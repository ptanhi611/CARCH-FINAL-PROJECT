/**
 * @file rvss_vm.h
 * @brief RVSS VM definition
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#pragma once


#include "vm/vm_base.h"

#include "rvss_control_unit.h"
#include "pipelining_register.h"
#include "vm/hazard_detection_unit.h"

#include "vm/branch_comparator_alu.h"

#include "vm/branch_prediction_unit.h"

#include <stack>
#include <vector>
#include <iostream>
#include <cstdint>

// #include <mutex>
// #include <condition_variable>
// #include <queue>
// #include <atomic>

// TODO: use a circular buffer instead of a stack for undo/redo

enum class MODES{
  SINGLE_CYLCE,
  PIPELINE_NO_HAZARD,
  PIPELINE_HAZARD,
  PIPELINE_HAZARD_FORWARDING,
  PIPELINE_H_F_STATIC_BRANCH,
  PIPELINE_H_F_DYNAMIC_BRANCH,
};

struct RegisterChange {
  unsigned int reg_index;
  unsigned int reg_type; // 0 for GPR, 1 for CSR, 2 for FPR
  uint64_t old_value;
  uint64_t new_value;
};

struct MemoryChange {
  uint64_t address;
  std::vector<uint8_t> old_bytes_vec; 
  std::vector<uint8_t> new_bytes_vec; 
};

struct StepDelta {
  uint64_t old_pc;
  uint64_t new_pc;
  std::vector<RegisterChange> register_changes;
  std::vector<MemoryChange> memory_changes;
};


// class RingUndoRedo {
//   std::vector<StepDelta> buffer_;
//   int current_;      // index of current state
//   int size_;         // number of valid entries
//   int head_;
//   const int capacity_;

//  public:
//   explicit RingUndoRedo(int cap)
//     : buffer_(cap), current_(-1), size_(0), head_(-1), capacity_(cap) {}

//   void push(const StepDelta& delta) {
//     head_ = (head_ + 1) % capacity_;
//     buffer_[head_] = delta;
//     current_ = head_;  // move current to new step

//     if (size_ < capacity_)
//         size_++;
//     else {
//         ; // overwrite oldest entry
//     }

//     // Invalidate all redos beyond head_
//     int i = (head_ + 1) % capacity_;
//     while (i != current_) {
//         buffer_[i] = StepDelta(); // or mark invalid
//         i = (i + 1) % capacity_;
//     }
// }


//   bool can_undo() const {
//     return size_ > 0 && current_ != -1;
//   }

//   bool can_redo() const {
//     return current_ != head_ && !buffer_[(current_ + 1) % capacity_].register_changes.empty();
// }

//   StepDelta undo() {
//     if (!can_undo()) throw std::runtime_error("Nothing to undo");
//     StepDelta delta = buffer_[current_];
//     current_ = (current_ - 1 + capacity_) % capacity_;
//     return delta;
//   }

//   StepDelta redo() {
//     if (!can_redo()) throw std::runtime_error("Nothing to redo");

//     current_ = (current_ + 1) % capacity_;
//     return buffer_[current_];
// }
// };




class RVSSVM : public VmBase {

  private:
    IF_ID_registers if_id_registers;
    ID_EX_registers id_ex_reg;
    EX_MEM_registers ex_mem;
    MEM_WB_registers mem_wb_reg;

    MODES pipeline_mode;



    IF_ID_registers if_id_registers_next;
    ID_EX_registers id_ex_reg_next;
    EX_MEM_registers ex_mem_next;
    MEM_WB_registers mem_wb_reg_next;


    bool pipeline_stalled_;
   
    bool running_;

    unsigned int stall_count ;


  

    HazardDetectionUnit HDU;
    RVSSControlUnit control_unit_;
    Branch_Comparision branch_alu;
    BranchPredictionUnit bpu_;

  public:
    std::atomic<bool> stop_requested_ = false;


    std::stack<StepDelta> undo_stack_;
    std::stack<StepDelta> redo_stack_;
    // RingUndoRedo history_{1000}; // or however many steps you want to store

    StepDelta current_delta_;

    // intermediate variables
    int64_t execution_result_{};
    int64_t memory_result_{};
    // int64_t memory_address_{};
    // int64_t memory_data_{};
    uint64_t return_address_{};

    bool branch_flag_ = false;
    bool flush_fetch_ = false;

    bool branch_taken  = false;
    int64_t next_pc_{}; // for jal, jalr,

    // CSR intermediate variables
    uint16_t csr_target_address_{};
    uint64_t csr_old_value_{};
    uint64_t csr_write_val_{};
    uint8_t csr_uimm_{};

    void Fetch();

    void Decode();

    void Execute();
    void ExecuteFloat();
    void ExecuteDouble();
    void ExecuteCsr();
    void HandleSyscall();

    void WriteMemory();
    void WriteMemoryFloat();
    void WriteMemoryDouble();

    void WriteBack();
    void WriteBackFloat();
    void WriteBackDouble();
    void WriteBackCsr();

    RVSSVM();
    ~RVSSVM();

    void Run() override;
    void DebugRun() override;
    void Step() override;
    void Undo() override;
    void Redo() override;
    void Reset() override;

    void Run_Pipelined();
    void Run_single();

    void Step_single();

    void Pipeline_fetch();
    void pipeline_decode();
    void pipeline_execute();
    void pipeline_mem();
    void pipeline_write_back();

    void pipeline_execute_float();
    void pipeline_execute_double();
    void pipeline_execute_csr();

    void DumpPipelineState(const std::filesystem::path);



  void Clocktick();

    void RequestStop() {
      stop_requested_ = true;
    }

    bool IsStopRequested() const {
      return stop_requested_;
    }
    
    void ClearStop() {
      stop_requested_ = false;
    }

    void PrintType() {
      std::cout << "rvssvm" << std::endl;
    }
};


