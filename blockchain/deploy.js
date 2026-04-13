const fs = require("fs");
const path = require("path");
const solc = require("solc");
const { ethers } = require("ethers");

// ─── Configuration ──────────────────────────────────────────────────────
const GANACHE_URL = process.env.GANACHE_URL || "http://127.0.0.1:7545";

// ─── Compile ────────────────────────────────────────────────────────────
function compile() {
  const contractPath = path.resolve(
    __dirname,
    "..",
    "contracts",
    "Voting.sol"
  );
  const source = fs.readFileSync(contractPath, "utf8");

  const input = {
    language: "Solidity",
    sources: { "Voting.sol": { content: source } },
    settings: {
      evmVersion: "london", // Ganache 2.7.x supports London EVM
      outputSelection: { "*": { "*": ["abi", "evm.bytecode.object"] } },
    },
  };

  const output = JSON.parse(solc.compile(JSON.stringify(input)));

  if (output.errors) {
    const fatal = output.errors.filter((e) => e.severity === "error");
    if (fatal.length > 0) {
      console.error("Compilation errors:");
      fatal.forEach((e) => console.error(e.formattedMessage));
      process.exit(1);
    }
    // Show warnings but continue
    output.errors
      .filter((e) => e.severity === "warning")
      .forEach((e) => console.warn(e.formattedMessage));
  }

  const contract = output.contracts["Voting.sol"]["Voting"];
  return {
    abi: contract.abi,
    bytecode: contract.evm.bytecode.object,
  };
}

// ─── Deploy ─────────────────────────────────────────────────────────────
async function deploy() {
  console.log("Compiling Voting.sol ...");
  const { abi, bytecode } = compile();
  console.log("Compilation successful!\n");

  // Save ABI for the server to use
  const artifactsDir = path.resolve(__dirname, "artifacts");
  if (!fs.existsSync(artifactsDir)) fs.mkdirSync(artifactsDir);
  fs.writeFileSync(
    path.join(artifactsDir, "Voting.json"),
    JSON.stringify({ abi, bytecode }, null, 2)
  );
  console.log("ABI saved to blockchain/artifacts/Voting.json\n");

  // Connect to Ganache
  const provider = new ethers.providers.JsonRpcProvider(GANACHE_URL);

  // Auto-detect account from Ganache (use the first account)
  const accounts = await provider.listAccounts();
  if (accounts.length === 0) {
    console.error("No accounts found in Ganache!");
    process.exit(1);
  }
  const signer = provider.getSigner(accounts[0]);
  const signerAddress = await signer.getAddress();

  console.log(`Deploying from account: ${signerAddress}`);
  const balance = await provider.getBalance(signerAddress);
  console.log(`Balance: ${ethers.utils.formatEther(balance)} ETH\n`);

  if (balance.eq(0)) {
    console.error("Account has 0 ETH! Make sure Ganache is running with funded accounts.");
    process.exit(1);
  }

  // Deploy using signer (Ganache unlocked accounts don't need private key)
  const factory = new ethers.ContractFactory(abi, bytecode, signer);
  const contract = await factory.deploy();
  await contract.deployed();

  console.log(`Voting contract deployed at: ${contract.address}\n`);

  // Add default candidates
  console.log("Adding default candidates ...");
  const candidateNames = [
    "Rahul Sharma",
    "Priya Patel",
    "Amit Kumar",
    "Sneha Desai",
  ];
  for (const name of candidateNames) {
    const tx = await contract.addCandidate(name);
    await tx.wait();
    console.log(`   Added: ${name}`);
  }

  // Auto-update server/.env with the contract address
  const envPath = path.resolve(__dirname, "..", "server", ".env");
  if (fs.existsSync(envPath)) {
    let envContent = fs.readFileSync(envPath, "utf8");
    envContent = envContent.replace(
      /CONTRACT_ADDRESS=.*/,
      `CONTRACT_ADDRESS=${contract.address}`
    );
    fs.writeFileSync(envPath, envContent);
    console.log("\n   Auto-updated server/.env with contract address!");
  }

  console.log("\n----------------------------------------------");
  console.log("  Deployment complete!");
  console.log(`  CONTRACT_ADDRESS = ${contract.address}`);
  console.log(`  GANACHE_URL      = ${GANACHE_URL}`);
  console.log("----------------------------------------------");
}

deploy().catch((err) => {
  console.error("Deployment failed:", err.message || err);
  process.exit(1);
});
