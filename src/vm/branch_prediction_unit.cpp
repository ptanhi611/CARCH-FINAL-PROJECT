#include "vm/branch_prediction_unit.h"
#include <iostream>

BranchPrediction BranchPredictionUnit::predict(uint64_t pc, uint64_t default_target) {
    BranchPrediction prediction;

    // --- GSHARE ALGORITHM ---
    // We XOR the PC with the Global History Register to create a context-sensitive index.
    // This allows the predictor to distinguish the same branch in different paths.
    uint64_t index = pc ^ global_history_;

    // 1. Check BHT
    if (bht_.find(index) == bht_.end()) {
        prediction.is_taken = false;
        bht_[index] = 1; // Initialize Weakly Not Taken
    } else {
        // 2-bit Counter: 0,1 = NT; 2,3 = Taken
        prediction.is_taken = (bht_[index] >= 2);
    }

    // 2. Check BTB
    if (prediction.is_taken) {
        if (btb_.find(pc) == btb_.end()) {
            prediction.target_pc = default_target;
            prediction.is_taken = false; // BTB Miss -> Force Not Taken
        } else {
            prediction.target_pc = btb_[pc]; // BTB Hit
        }
    } else {
        prediction.target_pc = default_target;
    }

    return prediction;
}

void BranchPredictionUnit::update(uint64_t pc, bool actual_outcome, uint64_t actual_target) {
    // --- GSHARE UPDATE ---
    uint64_t index = pc ^ global_history_;

    // 1. Update Saturating Counter
    int& counter = bht_[index]; 
    if (actual_outcome) {
        if (counter < 3) counter++; 
    } else {
        if (counter > 0) counter--; 
    }

    // 2. Update BTB
    if (actual_outcome) {
        btb_[pc] = actual_target;
    }

    // 3. Update Global History Register
    // Shift left and append new outcome (1=Taken, 0=Not Taken)
    global_history_ = (global_history_ << 1) | (actual_outcome ? 1 : 0);
}

void BranchPredictionUnit::recordPrediction(bool is_correct) {
    total_predictions_++;
    if (is_correct) {
        correct_predictions_++;
    }
}

double BranchPredictionUnit::getAccuracy() const {
    if (total_predictions_ == 0) return 0.0;
    return (double)correct_predictions_ / total_predictions_ * 100.0;
}

void BranchPredictionUnit::reset() {
    bht_.clear();
    btb_.clear();
    global_history_ = 0;
    total_predictions_ = 0;
    correct_predictions_ = 0;
}