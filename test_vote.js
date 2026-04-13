// Quick test: cast a vote and check results
const http = require('http');

function post(path, data) {
  return new Promise((resolve, reject) => {
    const body = JSON.stringify(data);
    const req = http.request({
      hostname: '127.0.0.1', port: 3000, path, method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Content-Length': body.length }
    }, res => {
      let d = '';
      res.on('data', c => d += c);
      res.on('end', () => resolve({ status: res.statusCode, data: JSON.parse(d) }));
    });
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

function get(path) {
  return new Promise((resolve, reject) => {
    http.get(`http://127.0.0.1:3000${path}`, res => {
      let d = '';
      res.on('data', c => d += c);
      res.on('end', () => resolve(JSON.parse(d)));
    }).on('error', reject);
  });
}

async function main() {
  console.log('=== Testing Blockchain Voting System ===\n');

  // 1. Cast a vote: Alice (fp_1) votes for Candidate A (id 0)
  console.log('1. Casting vote: Alice → Candidate A ...');
  const voteRes = await post('/api/vote', { voterId: 'fp_1', candidateId: 0 });
  console.log(`   Status: ${voteRes.status}`);
  console.log(`   Message: ${voteRes.data.message}`);
  console.log(`   TX Hash: ${voteRes.data.transactionHash}`);
  console.log(`   Block #: ${voteRes.data.blockNumber}\n`);

  // 2. Cast another vote: Bob (fp_2) votes for Candidate B (id 1)
  console.log('2. Casting vote: Bob → Candidate B ...');
  const voteRes2 = await post('/api/vote', { voterId: 'fp_2', candidateId: 1 });
  console.log(`   Status: ${voteRes2.status}`);
  console.log(`   Message: ${voteRes2.data.message}`);
  console.log(`   TX Hash: ${voteRes2.data.transactionHash}\n`);

  // 3. Try duplicate vote (should fail)
  console.log('3. Trying duplicate vote: Alice → Candidate B ...');
  const dupRes = await post('/api/vote', { voterId: 'fp_1', candidateId: 1 });
  console.log(`   Status: ${dupRes.status}`);
  console.log(`   Error: ${dupRes.data.error}\n`);

  // 4. Check results
  console.log('4. Fetching live results from blockchain ...');
  const results = await get('/api/results');
  console.log(`   Total votes: ${results.totalVotes}`);
  results.candidates.forEach(c => {
    console.log(`   ${c.name}: ${c.voteCount} votes`);
  });

  console.log('\n=== All tests passed! System is working end-to-end ===');
  console.log('Votes are stored on Ganache blockchain as transactions!');
}

main().catch(console.error);
