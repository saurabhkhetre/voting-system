// SPDX-License-Identifier: MIT
pragma solidity 0.8.17;

contract Voting {
    address public owner;

    struct Candidate {
        string name;
        uint256 voteCount;
    }

    Candidate[] public candidates;
    mapping(string => bool) public hasVoted; // voterId => voted?

    event CandidateAdded(uint256 candidateId, string name);
    event VoteCast(string voterId, uint256 candidateId);

    modifier onlyOwner() {
        require(msg.sender == owner, "Only owner can call this");
        _;
    }

    constructor() {
        owner = msg.sender;
    }

    /// @notice Add a new candidate (owner only)
    function addCandidate(string memory _name) public onlyOwner {
        candidates.push(Candidate({name: _name, voteCount: 0}));
        emit CandidateAdded(candidates.length - 1, _name);
    }

    /// @notice Cast a vote (one vote per voter ID)
    function vote(uint256 _candidateId, string memory _voterId) public {
        require(_candidateId < candidates.length, "Invalid candidate ID");
        require(!hasVoted[_voterId], "Voter has already voted");

        hasVoted[_voterId] = true;
        candidates[_candidateId].voteCount += 1;

        emit VoteCast(_voterId, _candidateId);
    }

    /// @notice Get candidate info by ID
    function getCandidate(uint256 _candidateId)
        public
        view
        returns (string memory name, uint256 voteCount)
    {
        require(_candidateId < candidates.length, "Invalid candidate ID");
        Candidate storage c = candidates[_candidateId];
        return (c.name, c.voteCount);
    }

    /// @notice Get total number of candidates
    function getCandidateCount() public view returns (uint256) {
        return candidates.length;
    }
}
