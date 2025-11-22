#include "vm/branch_prediction_unit.h"

#include "vm/branch_prediction_unit.h"


BranchPrediction BranchPredictionUnit::predict(uint64_t pc, uint64_t default_target) {
    BranchPrediction prediction;

    // 1. Check the Branch History Table (BHT)
    if (bht_.find(pc) == bht_.end()) {
        // Not in table, default to "Not Taken"
        prediction.is_taken = false;
        bht_[pc] = 0; // Initialize as Strongly Not Taken
    } else {
        // In table, predict based on the counter
        // Predict TAKEN if counter is 2 (10) or 3 (11)
        prediction.is_taken = (bht_[pc] >= 2);
    }

    // 2. Check the Branch Target Buffer (BTB)
    if (prediction.is_taken) {
        if (btb_.find(pc) == btb_.end()) {
            // We predict TAKEN, but don't know the target.
            // This is a BTB miss. We must fall back to PC+4
            // and let the pipeline stall/flush.
            // For simplicity, we'll just predict PC+4 for now.
            // A more complex design would stall here.
            prediction.target_pc = default_target;
            prediction.is_taken = false; // Can't predict taken without a target
        } else {
            // BTB Hit! We have a prediction AND a target.
            prediction.target_pc = btb_[pc];
        }
    } else {
        // We predict NOT TAKEN
        prediction.target_pc = default_target;
    }

    return prediction;
}

/**
 * @brief Updates the predictor with the actual branch outcome.
 */
void BranchPredictionUnit::update(uint64_t pc, bool actual_outcome, uint64_t actual_target) {
    // 1. Update the 2-bit Saturating Counter in the BHT
    int& counter = bht_[pc]; // Get or create the counter
    
    if (actual_outcome) {
        // Branch was TAKEN
        if (counter < 3) {
            counter++; // Saturate at 3 (Strongly Taken)
        }
    } else {
        // Branch was NOT TAKEN
        if (counter > 0) {
            counter--; // Saturate at 0 (Strongly Not Taken)
        }
    }

    // 2. Update the Branch Target Buffer (BTB)
    // If the branch was taken, store its target address
    if (actual_outcome) {
        btb_[pc] = actual_target;
    }
}

/**
 * @brief Resets all prediction tables.
 */
void BranchPredictionUnit::reset() {
    bht_.clear();
    btb_.clear();
}