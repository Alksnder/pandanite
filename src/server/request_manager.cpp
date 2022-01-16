#include <map>
#include <iostream>
#include <future>
#include "../core/helpers.hpp"
#include "../core/logger.hpp"
#include "../core/merkle_tree.hpp"
#include "request_manager.hpp"
using namespace std;

#define NEW_BLOCK_PEER_FANOUT 8

RequestManager::RequestManager(HostManager& hosts, string ledgerPath, string blockPath, string txdbPath) : hosts(hosts) {
    this->blockchain = new BlockChain(hosts, ledgerPath, blockPath, txdbPath);
    this->mempool = new MemPool(hosts, *this->blockchain);
    if (!hosts.isDisabled()) {
        this->blockchain->sync();
     
        // initialize the mempool with a random peers transactions:
        auto randomHost = hosts.sampleAllHosts(1);
        if (randomHost.size() > 0) {
            try {
                string host = *randomHost.begin();
                readRawTransactions(host, [&](Transaction t) {
                    mempool->addTransaction(t);
                });
            } catch(...) {}
        }
    }
    this->mempool->sync();
    this->blockchain->setMemPool(this->mempool);


}

void RequestManager::deleteDB() {
    this->blockchain->deleteDB();
}

json RequestManager::addTransaction(Transaction& t) {
    json result;
    result["status"] = executionStatusAsString(this->mempool->addTransaction(t));
    return result;
}

json RequestManager::submitProofOfWork(Block& newBlock) {
    json result;

    if (newBlock.getId() <= this->blockchain->getBlockCount()) {
        result["status"] = executionStatusAsString(INVALID_BLOCK_ID);
        return result;
    }
    // build map of all public keys in transaction
    this->blockchain->acquire();
    // add to the chain!
    ExecutionStatus status = this->blockchain->addBlock(newBlock);
    result["status"] = executionStatusAsString(status);
    this->blockchain->release();
    
    if (status == SUCCESS) {
        //pick random neighbor hosts and forward the new block to:
        set<string> neighbors = this->hosts.sampleFreshHosts(NEW_BLOCK_PEER_FANOUT);
        vector<future<void>> reqs;
        for(auto neighbor : neighbors) {
            std::thread{[neighbor, newBlock](){
                try {
                    Block a = newBlock;
                    submitBlock(neighbor, a);
                } catch(...) {
                    Logger::logStatus("Could not forward new block to " + neighbor);
                }
            }}.detach();
        }
    }
    
    return result;
}


json RequestManager::verifyTransaction(Transaction& t) {
    json response;
    Block b;
    try {
        uint32_t blockId = this->blockchain->findBlockForTransaction(t);
        response["status"] = "SUCCESS";
        response["blockId"] = blockId;
    } catch(...) {
        response["error"] = "Could not find block";
    }
    return response;
}

json RequestManager::getProofOfWork() {
    json result;
    vector<Transaction> transactions;
    result["lastHash"] = SHA256toString(this->blockchain->getLastHash());
    result["challengeSize"] = this->blockchain->getDifficulty();
    result["miningFee"] = this->blockchain->getCurrentMiningFee();
    BlockHeader last = this->blockchain->getBlockHeader(this->blockchain->getBlockCount());
    result["lastTimestamp"] = timeToString(last.timestamp);
    return result;

}

std::pair<uint8_t*, size_t> RequestManager::getRawBlockData(uint32_t blockId) {
    return this->blockchain->getRaw(blockId);
}

BlockHeader RequestManager::getBlockHeader(uint32_t blockId) {
    return this->blockchain->getBlockHeader(blockId);
}


std::pair<char*, size_t> RequestManager::getRawTransactionData() {
    return this->mempool->getRaw();
}

json RequestManager::getBlock(uint32_t blockId) {
    return this->blockchain->getBlock(blockId).toJson();
}

json RequestManager::getPeers() {
    json peers = json::array();
    for(auto h : this->hosts.getHosts()) {
        peers.push_back(h);
    }
    return peers;
}

json RequestManager::addPeer(string address, uint64_t time, string version) {
    this->hosts.addPeer(address, time, version);
    json ret;
    ret["status"] = executionStatusAsString(SUCCESS);
    return ret;
}


json RequestManager::getLedger(PublicWalletAddress w) {
    json result;
    Ledger& ledger = this->blockchain->getLedger();
    if (!ledger.hasWallet(w)) {
        result["error"] = "Wallet not found";
    } else {
        result["balance"] = ledger.getWalletValue(w);
    }
    return result;
}
string RequestManager::getBlockCount() {
    uint32_t count = this->blockchain->getBlockCount();
    return std::to_string(count);
}

string RequestManager::getTotalWork() {
    Bigint totalWork = this->blockchain->getTotalWork();
    return to_string(totalWork);
}

json RequestManager::getStats() {
    json info;
    if (this->blockchain->getBlockCount() == 1) {
        info["error"] = "Need more data";
        return info;
    }
    int coins = this->blockchain->getBlockCount()*50;
    info["num_coins"] = coins;
    info["num_wallets"] = 0;
    int blockId = this->blockchain->getBlockCount();
    info["pending_transactions"]= this->mempool->size();
    
    int idx = this->blockchain->getBlockCount();
    Block a = this->blockchain->getBlock(idx);
    Block b = this->blockchain->getBlock(idx-1);
    int timeDelta = a.getTimestamp() - b.getTimestamp();
    int totalSent = 0;
    int fees = 0;
    info["transactions"] = json::array();
    for(auto t : a.getTransactions()) {
        totalSent += t.getAmount();
        fees += t.getTransactionFee();
        info["transactions"].push_back(t.toJson());
    }
    int count = a.getTransactions().size();
    info["transactions_per_second"]= a.getTransactions().size()/(double)timeDelta;
    info["transaction_volume"]= totalSent;
    info["avg_transaction_size"]= totalSent/count;
    info["avg_transaction_fee"]= fees/count;
    info["difficulty"]= a.getDifficulty();
    info["current_block"]= a.getId();
    info["last_block_time"]= timeDelta;
    return info;
}
