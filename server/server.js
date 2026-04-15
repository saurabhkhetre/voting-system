const path = require("path");
require("dotenv").config({ path: path.resolve(__dirname, ".env") });
const express = require("express");
const cors = require("cors");
const { ethers } = require("ethers");
const fs = require("fs");
const crypto = require("crypto");

// ═══════════════════════════════════════════════════════════════
//  Configuration
// ═══════════════════════════════════════════════════════════════
const PORT = process.env.PORT || 3000;
const GANACHE_URL = process.env.GANACHE_URL || "http://127.0.0.1:7545";
const CONTRACT_ADDRESS = process.env.CONTRACT_ADDRESS;

// Admin credentials
const ADMIN_USERNAME = process.env.ADMIN_USERNAME || "saurabh";
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || "admin123";

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
//  Admin Session Management
// ═══════════════════════════════════════════════════════════════
const adminTokens = new Set();

function generateToken() {
  return crypto.randomBytes(32).toString("hex");
}

function requireAdmin(req, res, next) {
  const authHeader = req.headers.authorization;
  if (!authHeader || !authHeader.startsWith("Bearer ")) {
    return res.status(401).json({ success: false, error: "Authentication required" });
  }
  const token = authHeader.split(" ")[1];
  if (!adminTokens.has(token)) {
    return res.status(401).json({ success: false, error: "Invalid or expired token" });
  }
  next();
}

// ═══════════════════════════════════════════════════════════════
//  Device Heartbeat (ESP32 → Server → Dashboard)
// ═══════════════════════════════════════════════════════════════
let deviceStatus = {
  connected: false,
  lastHeartbeat: null,
  wifiRSSI: null,
  freeHeap: null,
  fingerprint: {
    connected: false,
    templateCount: 0,
    capacity: 0,
  },
  currentScreen: "",
  uptime: 0,
};

// ═══════════════════════════════════════════════════════════════
//  Enrollment Queue (browser → server → ESP32)
// ═══════════════════════════════════════════════════════════════
let enrollmentQueue = [];
let enrollmentIdCounter = 1;

// ═══════════════════════════════════════════════════════════════
//  Express App
// ═══════════════════════════════════════════════════════════════
const app = express();
app.use(cors());
app.use(express.json());

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
      "/api/admin/login",
      "/api/auth/fingerprint",
      "/api/register",
      "/api/voters",
      "/api/candidates",
      "/api/vote",
      "/api/results",
      "/api/admin/enroll",
      "/api/enroll/pending",
      "/api/enroll/complete",
      "/api/heartbeat",
      "/api/device/status",
    ],
  });
});

// ═══════════════════════════════════════════════════════════════
//  Device Heartbeat Endpoints
// ═══════════════════════════════════════════════════════════════

// ─── POST /api/heartbeat ──────────────────────────────────────
// ESP32 sends this every 5 seconds with its status
app.post("/api/heartbeat", (req, res) => {
  const { wifiRSSI, freeHeap, fingerprint, currentScreen, uptime } = req.body;

  deviceStatus.connected = true;
  deviceStatus.lastHeartbeat = new Date().toISOString();
  deviceStatus.wifiRSSI = wifiRSSI || null;
  deviceStatus.freeHeap = freeHeap || null;
  deviceStatus.currentScreen = currentScreen || "";
  deviceStatus.uptime = uptime || 0;

  if (fingerprint) {
    deviceStatus.fingerprint.connected = fingerprint.connected || false;
    deviceStatus.fingerprint.templateCount = fingerprint.templateCount || 0;
    deviceStatus.fingerprint.capacity = fingerprint.capacity || 0;
  }

  return res.json({ success: true });
});

// ─── GET /api/device/status ───────────────────────────────────
// Dashboard polls this to show hardware indicators
app.get("/api/device/status", (req, res) => {
  // If last heartbeat was more than 10 seconds ago, mark as disconnected
  if (deviceStatus.lastHeartbeat) {
    const elapsed = Date.now() - new Date(deviceStatus.lastHeartbeat).getTime();
    if (elapsed > 10000) {
      deviceStatus.connected = false;
    }
  }

  return res.json({
    success: true,
    device: deviceStatus,
  });
});

// ═══════════════════════════════════════════════════════════════
//  Admin Authentication
// ═══════════════════════════════════════════════════════════════

// ─── POST /api/admin/login ─────────────────────────────────────
app.post("/api/admin/login", (req, res) => {
  const { username, password } = req.body;

  if (!username || !password) {
    return res.status(400).json({ success: false, error: "Username and password are required" });
  }

  if (username.toLowerCase() !== ADMIN_USERNAME.toLowerCase() || password !== ADMIN_PASSWORD) {
    return res.status(401).json({ success: false, error: "Invalid credentials" });
  }

  const token = generateToken();
  adminTokens.add(token);

  console.log(`Admin '${username}' logged in`);

  return res.json({
    success: true,
    token,
    admin: { username: ADMIN_USERNAME, name: "Saurabh" },
    message: "Admin login successful",
  });
});

// ─── POST /api/admin/logout ────────────────────────────────────
app.post("/api/admin/logout", requireAdmin, (req, res) => {
  const token = req.headers.authorization.split(" ")[1];
  adminTokens.delete(token);
  return res.json({ success: true, message: "Logged out" });
});

// ─── GET /api/admin/verify ─────────────────────────────────────
app.get("/api/admin/verify", requireAdmin, (req, res) => {
  return res.json({ success: true, admin: { username: ADMIN_USERNAME, name: "Saurabh" } });
});

// ═══════════════════════════════════════════════════════════════
//  Enrollment Queue (Admin triggers from browser, ESP32 executes)
// ═══════════════════════════════════════════════════════════════

// ─── POST /api/admin/enroll ────────────────────────────────────
// Admin submits a new voter for enrollment (from browser)
app.post("/api/admin/enroll", requireAdmin, (req, res) => {
  const { name, userId } = req.body;

  if (!name || !userId) {
    return res.status(400).json({ success: false, error: "name and userId are required" });
  }

  // Check if there's already an active enrollment
  const active = enrollmentQueue.find(e => e.status === "pending" || e.status === "enrolling");
  if (active) {
    return res.status(409).json({
      success: false,
      error: "Another enrollment is already in progress",
      activeEnrollment: active,
    });
  }

  const enrollment = {
    id: enrollmentIdCounter++,
    name: name.trim(),
    userId: userId.trim(),
    status: "pending", // pending → enrolling → completed / failed
    fingerprintId: null,
    createdAt: new Date().toISOString(),
    completedAt: null,
    error: null,
  };

  enrollmentQueue.push(enrollment);
  console.log(`Enrollment queued: ${enrollment.name} (ID: ${enrollment.userId})`);

  return res.json({
    success: true,
    enrollment,
    message: "Enrollment request queued. Ask voter to place finger on sensor.",
  });
});

// ─── GET /api/admin/enroll/:id/status ──────────────────────────
// Browser polls this to check enrollment progress
app.get("/api/admin/enroll/:id/status", requireAdmin, (req, res) => {
  const id = parseInt(req.params.id);
  const enrollment = enrollmentQueue.find(e => e.id === id);

  if (!enrollment) {
    return res.status(404).json({ success: false, error: "Enrollment not found" });
  }

  return res.json({ success: true, enrollment });
});

// ─── DELETE /api/admin/enroll/:id ──────────────────────────────
// Cancel a pending enrollment
app.delete("/api/admin/enroll/:id", requireAdmin, (req, res) => {
  const id = parseInt(req.params.id);
  const idx = enrollmentQueue.findIndex(e => e.id === id);

  if (idx === -1) {
    return res.status(404).json({ success: false, error: "Enrollment not found" });
  }

  enrollmentQueue[idx].status = "cancelled";
  console.log(`Enrollment cancelled: ${enrollmentQueue[idx].name}`);

  return res.json({ success: true, message: "Enrollment cancelled" });
});

// ─── GET /api/enroll/pending ───────────────────────────────────
// ESP32 polls this endpoint to check for pending enrollments
app.get("/api/enroll/pending", (req, res) => {
  const pending = enrollmentQueue.find(e => e.status === "pending");

  if (!pending) {
    return res.json({ success: true, hasPending: false });
  }

  // Mark as "enrolling" so it won't be picked up again
  pending.status = "enrolling";
  console.log(`ESP32 picked up enrollment: ${pending.name}`);

  return res.json({
    success: true,
    hasPending: true,
    enrollment: {
      id: pending.id,
      name: pending.name,
      userId: pending.userId,
    },
  });
});

// ─── POST /api/enroll/complete ─────────────────────────────────
// ESP32 calls this after fingerprint enrollment is done
app.post("/api/enroll/complete", (req, res) => {
  const { enrollmentId, fingerprintId, success: wasSuccessful, error: errorMsg } = req.body;

  if (enrollmentId === undefined || enrollmentId === null) {
    return res.status(400).json({ success: false, error: "enrollmentId is required" });
  }

  const enrollment = enrollmentQueue.find(e => e.id === enrollmentId);
  if (!enrollment) {
    return res.status(404).json({ success: false, error: "Enrollment not found" });
  }

  if (wasSuccessful && fingerprintId) {
    // Register the voter
    const voterId = `fp_${fingerprintId}`;

    // Check if fingerprint already used
    if (VOTERS[voterId]) {
      enrollment.status = "failed";
      enrollment.error = `Fingerprint ID ${fingerprintId} already registered to ${VOTERS[voterId].name}`;
      enrollment.completedAt = new Date().toISOString();
      return res.json({ success: false, error: enrollment.error });
    }

    VOTERS[voterId] = { name: enrollment.name, fingerprintId: parseInt(fingerprintId) };
    saveVoters(VOTERS);

    enrollment.status = "completed";
    enrollment.fingerprintId = parseInt(fingerprintId);
    enrollment.completedAt = new Date().toISOString();

    console.log(`Enrollment completed: ${enrollment.name} → FP#${fingerprintId}`);

    return res.json({
      success: true,
      message: `Voter "${enrollment.name}" registered with fingerprint ${fingerprintId}`,
      voter: { voterId, name: enrollment.name, fingerprintId: parseInt(fingerprintId) },
    });
  } else {
    enrollment.status = "failed";
    enrollment.error = errorMsg || "Enrollment failed on device";
    enrollment.completedAt = new Date().toISOString();

    console.log(`Enrollment failed: ${enrollment.name} — ${enrollment.error}`);

    return res.json({ success: false, error: enrollment.error });
  }
});

// ═══════════════════════════════════════════════════════════════
//  Existing Endpoints (fingerprint auth, voting, etc.)
// ═══════════════════════════════════════════════════════════════

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
    console.log("  👤 Admin Panel: Browser-based");
    console.log(`  Server:   http://localhost:${PORT}`);
    console.log(`  Ganache:  ${GANACHE_URL}`);
    console.log(`  Contract: ${CONTRACT_ADDRESS}`);
    console.log(`  Admin:    ${ADMIN_USERNAME} / ***`);
    console.log("==============================================");
    console.log("\nEndpoints:");
    console.log("  POST /api/admin/login        - Admin login");
    console.log("  POST /api/admin/logout       - Admin logout");
    console.log("  GET  /api/admin/verify       - Verify admin token");
    console.log("  POST /api/admin/enroll       - Queue fingerprint enrollment");
    console.log("  GET  /api/admin/enroll/:id    - Check enrollment status");
    console.log("  GET  /api/enroll/pending     - ESP32 polls for enrollments");
    console.log("  POST /api/enroll/complete    - ESP32 reports enrollment done");
    console.log("  POST /api/auth/fingerprint   - Authenticate via fingerprint");
    console.log("  POST /api/register           - Register a new voter");
    console.log("  DELETE /api/register/:id     - Delete a voter");
    console.log("  GET  /api/voters             - List all registered voters");
    console.log("  GET  /api/candidates         - List candidates");
    console.log("  POST /api/vote               - Cast a vote");
    console.log("  GET  /api/results            - Get live results");
    console.log("  POST /api/heartbeat          - ESP32 heartbeat");
    console.log("  GET  /api/device/status      - Device status for dashboard\n");

    // Show registered voters on startup
    const voterCount = Object.keys(VOTERS).length;
    console.log(`  Registered voters: ${voterCount}`);
    for (const [id, voter] of Object.entries(VOTERS)) {
      console.log(`    ${id} → ${voter.name} (FP#${voter.fingerprintId})`);
    }
    console.log("");
  });
}

// Handle malformed JSON requests gracefully (must be after routes)
app.use((err, req, res, next) => {
  if (err.type === "entity.parse.failed") {
    return res.status(400).json({ success: false, error: "Invalid JSON in request body" });
  }
  next(err);
});

start().catch((err) => {
  console.error("Failed to start server:", err.message);
  process.exit(1);
});
