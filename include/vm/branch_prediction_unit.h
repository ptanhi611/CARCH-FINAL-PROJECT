#ifndef BRANCH_PREDICTION_UNIT_H
#define BRANCH_PREDICTION_UNIT_H

#include <cstdint>
#include <map>

struct BranchPrediction {
    bool is_taken = false;
    uint64_t target_pc = 0;
};

class BranchPredictionUnit {
public:
    // Predicts outcome based on PC and Global History (Gshare)
    BranchPrediction predict(uint64_t pc, uint64_t default_target);
    
    // Updates BHT, BTB, and Global History
    void update(uint64_t pc, bool actual_outcome, uint64_t actual_target);
    
    // Records statistics (Called from Decode stage)
    void recordPrediction(bool is_correct);
    
    void reset();
    
    // Get current accuracy percentage (0.0 - 100.0)
    double getAccuracy() const;

private:
    // Branch History Table (Map simulates sparse table)
    std::map<uint64_t, int> bht_; 
    
    // Branch Target Buffer
    std::map<uint64_t, uint64_t> btb_;

    // Gshare: Global History Register (Stores outcome of last N branches)
    uint64_t global_history_ = 0;

    // Statistics
    uint64_t total_predictions_ = 0;
    uint64_t correct_predictions_ = 0;
};

#endif // BRANCH_PREDICTION_UNIT_H