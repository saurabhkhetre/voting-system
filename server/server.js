const path = require("path");
require("dotenv").config({ path: path.resolve(__dirname, ".env") });
const express = require("express");
const cors = require("cors");
const { ethers } = require("ethers");
const fs = require("fs");

// ═══════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════
const PORT = process.env.PORT || 3000;
const GANACHE_URL = process.env.GANACHE_URL || "http://127.0.0.1:7545";
const CONTRACT_ADDRESS = process.env.CONTRACT_ADDRESS;

if (!CONTRACT_ADDRESS || CONTRACT_ADDRESS === "PASTE_YOUR_CONTRACT_ADDRESS_HERE") {
  console.error("Missing CONTRACT_ADDRESS in server/.env");
  console.error("Run  'node blockchain/deploy.js'  first, then paste the address into .env");
  process.exit(1);
}

// ═══════════════════════════════════════════════════════════════
//  Blockchain Setup
// ═══════════════════════════════════════════════════════════════
const provider = new ethers.providers.JsonRpcProvider(GANACHE_URL);

// Load ABI from the artifact produced by deploy.js
const artifactPath = path.resolve(__dirname, "..", "blockchain", "artifacts", "Voting.json");
if (!fs.existsSync(artifactPath)) {
  console.error("ABI artifact not found at:", artifactPath);
  console.error("Run  'node blockchain/deploy.js'  first.");
  process.exit(1);
}
const { abi } = JSON.parse(fs.readFileSync(artifactPath, "utf8"));

// We'll initialize the contract with signer after connecting
let votingContract;

async function initContract() {
  // Use Ganache's first unlocked account (no private key needed)
  const accounts = await provider.listAccounts();
  const signer = provider.getSigner(accounts[0]);
  votingContract = new ethers.Contract(CONTRACT_ADDRESS, abi, signer);
  console.log(`Using account: ${accounts[0]}`);
}

// ═══════════════════════════════════════════════════════════════
//  Voter Database (Fingerprint ID based, persisted to JSON)
// ═══════════════════════════════════════════════════════════════
const VOTERS_FILE = path.resolve(__dirname, "voters.json");

function loadVoters() {
  try {
    if (fs.existsSync(VOTERS_FILE)) {
      const data = JSON.parse(fs.readFileSync(VOTERS_FILE, "utf8"));
      return data.voters || {};
    }
  } catch (err) {
    console.error("Error loading voters.json:", err.message);
  }
  return {};
}

function saveVoters(voters) {
  try {
    fs.writeFileSync(VOTERS_FILE, JSON.stringify({ voters }, null, 2));
  } catch (err) {
    console.error("Error saving voters.json:", err.message);
  }
}

let VOTERS = loadVoters();

// Track which voters have voted (in-memory, mirrors on-chain state)
const votedLocally = new Set();

// ═══════════════════════════════════════════════════════════════
//  Express App
// ═══════════════════════════════════════════════════════════════
const app = express();
app.use(cors());
app.use(express.json());

// Handle malformed JSON requests gracefully
app.use((err, req, res, next) => {
  if (err.type === "entity.parse.failed") {
    return res.status(400).json({ success: false, error: "Invalid JSON in request body" });
  }
  next(err);
});

// Serve the browser UI from public/
app.use(express.static(path.join(__dirname, "public")));

// ─── Health Check ───────────────────────────────────────────────
app.get("/api/health", (req, res) => {
  res.json({
    status: "ok",
    message: "Blockchain Voting System API (Fingerprint Auth)",
    contractAddress: CONTRACT_ADDRESS,
    ganacheUrl: GANACHE_URL,
    endpoints: [
      "/api/auth/fingerprint",
      "/api/register",
      "/api/voters",
      "/api/candidates",
      "/api/vote",
      "/api/results",
    ],
  });
});

// ─── POST /api/auth/fingerprint ─────────────────────────────────
// ESP32 sends the fingerprint ID after a successful scan
app.post("/api/auth/fingerprint", async (req, res) => {
  const { fingerprintId } = req.body;

  if (fingerprintId === undefined || fingerprintId === null) {
    return res.status(400).json({ success: false, error: "fingerprintId is required" });
  }

  const voterId = `fp_${fingerprintId}`;
  const voter = VOTERS[voterId];

  if (!voter) {
    return res.status(404).json({
      success: false,
      error: "Fingerprint not registered. Please register first.",
    });
  }

  // Check blockchain state (survives server restarts)
  try {
    const alreadyVotedOnChain = await votingContract.hasVoted(voterId);
    if (alreadyVotedOnChain) {
      votedLocally.add(voterId);
      return res.status(403).json({
        success: false,
        error: "You have already voted",
        alreadyVoted: true,
        voter: { voterId, name: voter.name },
      });
    }
  } catch (err) {
    console.error("Error checking blockchain vote status:", err.message);
    if (votedLocally.has(voterId)) {
      return res.status(403).json({
        success: false,
        error: "You have already voted",
        alreadyVoted: true,
        voter: { voterId, name: voter.name },
      });
    }
  }

  return res.json({
    success: true,
    voter: { voterId, name: voter.name, fingerprintId: voter.fingerprintId },
    message: "Authentication successful",
  });
});

// ─── POST /api/login (kept for web dashboard compatibility) ─────
app.post("/api/login", async (req, res) => {
  const { voterId } = req.body;

  if (!voterId) {
    return res.status(400).json({ success: false, error: "voterId is required" });
  }

  const voter = VOTERS[voterId];
  if (!voter) {
    return res.status(401).json({ success: false, error: "Voter not found" });
  }

  // Check blockchain state
  try {
    const alreadyVotedOnChain = await votingContract.hasVoted(voterId);
    if (alreadyVotedOnChain) {
      votedLocally.add(voterId);
      return res.status(403).json({ success: false, error: "You have already voted", alreadyVoted: true });
    }
  } catch (err) {
    console.error("Error checking blockchain vote status:", err.message);
    if (votedLocally.has(voterId)) {
      return res.status(403).json({ success: false, error: "You have already voted", alreadyVoted: true });
    }
  }

  return res.json({
    success: true,
    voter: { voterId, name: voter.name },
    message: "Login successful",
  });
});

// ─── POST /api/register ────────────────────────────────────────
// Register a new voter with a fingerprint ID
app.post("/api/register", (req, res) => {
  const { fingerprintId, name } = req.body;

  if (!fingerprintId || !name) {
    return res.status(400).json({ success: false, error: "fingerprintId and name are required" });
  }

  const voterId = `fp_${fingerprintId}`;

  // Check if this fingerprint ID is already registered
  if (VOTERS[voterId]) {
    return res.status(409).json({
      success: false,
      error: `Fingerprint ID ${fingerprintId} is already registered to ${VOTERS[voterId].name}`,
    });
  }

  // Register the voter
  VOTERS[voterId] = { name: name.trim(), fingerprintId: parseInt(fingerprintId) };
  saveVoters(VOTERS);

  console.log(`Registered voter: ${name} (fingerprint ID: ${fingerprintId})`);

  return res.json({
    success: true,
    message: `Voter "${name}" registered successfully`,
    voter: { voterId, name: name.trim(), fingerprintId: parseInt(fingerprintId) },
  });
});

// ─── DELETE /api/register/:voterId ──────────────────────────────
// Delete a registered voter
app.delete("/api/register/:voterId", (req, res) => {
  const { voterId } = req.params;

  if (!VOTERS[voterId]) {
    return res.status(404).json({ success: false, error: "Voter not found" });
  }

  const name = VOTERS[voterId].name;
  delete VOTERS[voterId];
  saveVoters(VOTERS);

  console.log(`Deleted voter: ${name} (${voterId})`);

  return res.json({ success: true, message: `Voter "${name}" deleted` });
});

// ─── GET /api/voters ────────────────────────────────────────────
// List all registered voters
app.get("/api/voters", async (req, res) => {
  const voterList = [];

  for (const [voterId, voter] of Object.entries(VOTERS)) {
    let hasVoted = votedLocally.has(voterId);

    // Also check blockchain
    if (!hasVoted) {
      try {
        hasVoted = await votingContract.hasVoted(voterId);
        if (hasVoted) votedLocally.add(voterId);
      } catch (err) {
        // ignore
      }
    }

    voterList.push({
      voterId,
      name: voter.name,
      fingerprintId: voter.fingerprintId,
      hasVoted,
    });
  }

  return res.json({ success: true, voters: voterList });
});

// ─── GET /api/candidates ───────────────────────────────────────
app.get("/api/candidates", async (req, res) => {
  try {
    const count = await votingContract.getCandidateCount();
    const candidates = [];

    for (let i = 0; i < count.toNumber(); i++) {
      const [name, voteCount] = await votingContract.getCandidate(i);
      candidates.push({ id: i, name, voteCount: voteCount.toNumber() });
    }

    return res.json({ success: true, candidates });
  } catch (err) {
    console.error("Error fetching candidates:", err.message);
    return res.status(500).json({ success: false, error: "Failed to fetch candidates" });
  }
});

// ─── POST /api/vote ─────────────────────────────────────────────
app.post("/api/vote", async (req, res) => {
  const { voterId, candidateId } = req.body;

  if (voterId === undefined || candidateId === undefined) {
    return res.status(400).json({ success: false, error: "voterId and candidateId are required" });
  }

  // Verify voter exists
  if (!VOTERS[voterId]) {
    return res.status(401).json({ success: false, error: "Unknown voter" });
  }

  // Check local voted set
  if (votedLocally.has(voterId)) {
    return res.status(403).json({ success: false, error: "You have already voted", alreadyVoted: true });
  }

  try {
    console.log(`Vote received: voter=${voterId}, candidate=${candidateId}`);

    // Send transaction to blockchain
    const tx = await votingContract.vote(candidateId, voterId);
    console.log(`Transaction sent: ${tx.hash}`);
    const receipt = await tx.wait();
    console.log(`Transaction confirmed in block ${receipt.blockNumber}`);

    // Mark as voted locally
    votedLocally.add(voterId);

    // Fetch updated results for this candidate
    const [name, voteCount] = await votingContract.getCandidate(candidateId);

    return res.json({
      success: true,
      message: "Vote recorded on blockchain!",
      transactionHash: tx.hash,
      blockNumber: receipt.blockNumber,
      candidate: { id: candidateId, name, voteCount: voteCount.toNumber() },
    });
  } catch (err) {
    console.error("Voting error:", err.message);

    // If the smart contract rejected it (already voted on-chain)
    if (err.message.includes("already voted")) {
      votedLocally.add(voterId);
      return res.status(403).json({ success: false, error: "Voter has already voted (on-chain)", alreadyVoted: true });
    }

    return res.status(500).json({ success: false, error: "Transaction failed: " + err.message });
  }
});

// ─── GET /api/results ──────────────────────────────────────────
app.get("/api/results", async (req, res) => {
  try {
    const count = await votingContract.getCandidateCount();
    const candidates = [];
    let totalVotes = 0;

    for (let i = 0; i < count.toNumber(); i++) {
      const [name, voteCount] = await votingContract.getCandidate(i);
      const votes = voteCount.toNumber();
      candidates.push({ id: i, name, voteCount: votes });
      totalVotes += votes;
    }

    return res.json({ success: true, totalVotes, candidates });
  } catch (err) {
    console.error("Error fetching results:", err.message);
    return res.status(500).json({ success: false, error: "Failed to fetch results" });
  }
});

// ═══════════════════════════════════════════════════════════════
//  Start Server
// ═══════════════════════════════════════════════════════════════
async function start() {
  await initContract();

  app.listen(PORT, () => {
    console.log("==============================================");
    console.log("  Blockchain Voting System - Backend");
    console.log("  🔐 Fingerprint Authentication Enabled");
    console.log(`  Server:   http://localhost:${PORT}`);
    console.log(`  Ganache:  ${GANACHE_URL}`);
    console.log(`  Contract: ${CONTRACT_ADDRESS}`);
    console.log("==============================================");
    console.log("\nEndpoints:");
    console.log("  POST /api/auth/fingerprint  - Authenticate via fingerprint");
    console.log("  POST /api/register          - Register a new voter");
    console.log("  DELETE /api/register/:id     - Delete a voter");
    console.log("  GET  /api/voters            - List all registered voters");
    console.log("  GET  /api/candidates        - List candidates");
    console.log("  POST /api/vote              - Cast a vote");
    console.log("  GET  /api/results           - Get live results\n");

    // Show registered voters on startup
    const voterCount = Object.keys(VOTERS).length;
    console.log(`  Registered voters: ${voterCount}`);
    for (const [id, voter] of Object.entries(VOTERS)) {
      console.log(`    ${id} → ${voter.name} (FP#${voter.fingerprintId})`);
    }
    console.log("");
  });
}

start().catch((err) => {
  console.error("Failed to start server:", err.message);
  process.exit(1);
});
