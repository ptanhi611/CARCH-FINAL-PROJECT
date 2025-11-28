/**
 * @file rvss_vm.cpp
 * @brief RVSS VM implementation
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#include "vm/rvss/rvss_vm.h"

#include "utils.h"
#include "globals.h"
#include "common/instructions.h"
#include "config.h"
#include "vm/alu.h" 
#include "vm/registers.h" 

#include <cctype>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <stack>  
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <cstring> 
#include <limits> 
#include <iomanip> 

using instruction_set::Instruction;
using instruction_set::get_instr_encoding;

// Constructor: Initialize modes and registers
RVSSVM::RVSSVM() : VmBase() {
  // Read mode from config
  int mode_from_config = vm_config::config.getPipelineMode();
  pipeline_mode = static_cast<MODES>(mode_from_config);
  
  pipeline_stalled_ = false;    //bool for checkinf whether it has stalled or not
  stall_count = 0;              //counter
  flush_fetch_ = false;         
  running_ = false;

  id_ex_reg = {};               //declaration of all pipeline registers
  if_id_registers = {};
  ex_mem = {};
  mem_wb_reg = {};

  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

RVSSVM::~RVSSVM() = default;


void RVSSVM::DumpPipelineState(const std::filesystem::path filename) {
    std::ofstream file(filename);
    if (!file.is_open()) return;

    file << "{\n";
    // Basic Stats
    file << "  \"cycle\": " << cycle_s_ << ",\n";
    file << "  \"pc\": " << program_counter_ << ",\n";
    file << "  \"stall_count\": " << stall_count << ",\n";
    file << "  \"program_counter_hex\": \"0x" << std::hex << program_counter_ << std::dec << "\",\n";

    // Pipeline State Object
    file << "  \"pipeline\": {\n";
    
    // FETCH Stage
    file << "    \"fetch\": {\n";
    file << "      \"pc\": " << if_id_registers_next.pc << ",\n";
    file << "      \"inst_hex\": " << if_id_registers_next.inst << ",\n";
    file << "      \"desc\": \"" << (if_id_registers_next.valid ? "BUSY" : "BUBBLE") << "\"\n";
    file << "    },\n";

    // DECODE Stage
    file << "    \"decode\": {\n";
    file << "      \"pc\": " << if_id_registers.pc << ",\n";
    file << "      \"inst_hex\": " << if_id_registers.inst << ",\n";
    file << "      \"desc\": \"" << (if_id_registers.valid ? "BUSY" : "BUBBLE") << "\"\n";
    file << "    },\n";

    // EXECUTE Stage
    file << "    \"execute\": {\n";
    file << "      \"inst_hex\": " << id_ex_reg.instruction_bits << ",\n";
    file << "      \"rd\": " << (int)id_ex_reg.rd << ",\n";
    file << "      \"forward_a\": " << (int)id_ex_reg.forward_A << ",\n";
    file << "      \"forward_b\": " << (int)id_ex_reg.forward_B << ",\n";
    file << "      \"desc\": \"" << (id_ex_reg.valid ? "BUSY" : "BUBBLE") << "\"\n";
    file << "    },\n";

    // MEMORY Stage
    file << "    \"memory\": {\n";
    file << "      \"inst_hex\": " << ex_mem.instruction_bits << ",\n";
    file << "      \"alu_result\": " << ex_mem.alu_ans << ",\n";
    file << "      \"desc\": \"" << (ex_mem.valid ? "BUSY" : "BUBBLE") << "\"\n";
    file << "    },\n";

    // WRITEBACK Stage
    file << "    \"writeback\": {\n";
    file << "      \"inst_hex\": " << mem_wb_reg.instruction_bits << ",\n";
    file << "      \"wb_value\": " << (mem_wb_reg.signals.mem_to_reg ? mem_wb_reg.mem_data : mem_wb_reg.alu_ans) << ",\n";
    file << "      \"desc\": \"" << (mem_wb_reg.valid ? "BUSY" : "BUBBLE") << "\"\n";
    file << "    }\n";

    file << "  }\n"; // End pipeline object
    file << "}\n";
    file.close();
}

void RVSSVM::Clocktick(){
  std::cout << "--- CYCLE " << std::dec << cycle_s_ + 1 << " START --- PC: 0x" << std::hex << program_counter_ << std::endl;


  if_id_registers_next = {};
  id_ex_reg_next = {};
  ex_mem_next = {};
  mem_wb_reg_next = {};


  pipeline_write_back();
  pipeline_mem();
  pipeline_execute();
  pipeline_decode();
  Pipeline_fetch();



  if_id_registers = if_id_registers_next;
  id_ex_reg = id_ex_reg_next;
  ex_mem = ex_mem_next;
  mem_wb_reg = mem_wb_reg_next;

    cycle_s_++;

  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpPipelineState(globals::vm_state_dump_file_path);
}



void RVSSVM::Pipeline_fetch() {

  if (flush_fetch_) {
      flush_fetch_ = false;
      if_id_registers_next.valid = false; // Inject NOP
      std::cout << "[FETCH] FLUSHED (Branch Mispredict)" << std::endl;
      return; 
  }
 
  else if (pipeline_stalled_) {
    pipeline_stalled_ = false;  // Reset for next cycle
    
    if_id_registers_next = if_id_registers; 
    std::cout << "[FETCH] STALLED" << std::endl;
    return; // Do NOT fetch, do NOT advance PC
  }

 
  uint64_t current_pc = program_counter_;
  uint64_t next_pc = program_counter_ + 4;
  uint64_t predicted_pc = next_pc;

  bool predicted_taken = false;

  if(pipeline_mode>=MODES::PIPELINE_H_F_DYNAMIC_BRANCH){
    BranchPrediction prediction = bpu_.predict(current_pc,next_pc);
    predicted_pc = prediction.target_pc;
    predicted_taken = prediction.is_taken;

    std::cout << "[FETCH] BPU Predicts: " << (predicted_taken ? "TAKEN" : "NOT TAKEN") << " to 0x" << std::hex << predicted_pc << std::dec << std::endl;
  }

   std::cout << "[FETCH] PC: 0x" << std::hex << program_counter_ << std::dec << std::endl;


  try {
    if_id_registers_next.inst = memory_controller_.ReadWord(current_pc);
    if_id_registers_next.pc = current_pc;
    if_id_registers_next.pc_plus_4 = next_pc;
    if_id_registers_next.valid = true; // Mark as stall instruction
    if_id_registers_next.predicted_as_taken = predicted_taken;

  } catch (const std::runtime_error& e) {
    if_id_registers_next.inst = 0; // Inject NOP on memory fail
    if_id_registers_next.valid = false;
  }
  
  program_counter_ = predicted_pc;
}

void RVSSVM::pipeline_decode() {
  // 1. Get the current mode
  int mode = static_cast<int>(pipeline_mode);
  
  // 2. Ask the HDU
  PipelineControlSignals signals = HDU.compute_signals(
      mode,
      if_id_registers.inst, 
      id_ex_reg,      
      ex_mem,     
      mem_wb_reg     
  );

  // 3. React to HDU signals
  if (signals.stall_fetch) {
      pipeline_stalled_ = true; 
      stall_count++; 
      
      id_ex_reg_next = {}; 
      id_ex_reg_next.valid = false;
      // You can add a log here if you want, but original didn't have a specific stall log inside decode
      return;
  }
  if (signals.flush_decode) {
      std::cout << "[DECODE] FLUSHED by HDU" << std::endl;
      id_ex_reg_next = {}; 
      id_ex_reg_next.valid = false;
      return;
  }

  // 4. Bubble Check
  if (!if_id_registers.valid) {
      std::cout << "[DECODE] Bubble passing through" << std::endl;
      id_ex_reg_next = {};
      id_ex_reg_next.valid = false;
      return;
  }

  // 5. Decode
  const uint32_t instruction = if_id_registers.inst;
  std::cout << "[DECODE] OK. Decoding 0x" << std::hex << instruction << std::dec << std::endl;
  
  control_unit_.SetControlSignals(instruction);

  uint8_t opcode = instruction & 0b1111111;
  uint8_t rs1 = (instruction >> 15) & 0b11111;
  uint8_t rs2 = (instruction >> 20) & 0b11111;
  uint8_t rd = (instruction >> 7) & 0b11111;
  uint8_t funct3 = (instruction >> 12) & 0b111;
  uint8_t funct7 = (instruction >> 25) & 0b1111111;
  int32_t imm = ImmGenerator(instruction);

  // --- TYPE DETECTION (Fix for Float/Double Reg Reading) ---
  bool is_float = instruction_set::isFInstruction(instruction);
  bool is_double = instruction_set::isDInstruction(instruction);
  bool rs1_is_fp = false;
  bool rs2_is_fp = false;

  if (is_float || is_double) {
      // FSW/FSD and FMV.W.X use Integer RS1
      bool is_store = (opcode == 0b0100111); 
      bool is_fmv_x = (opcode == 0b1010011 && funct7 == 0b1110000); 

      if (is_store || is_fmv_x) rs1_is_fp = false; 
      else rs1_is_fp = true;  
      
      rs2_is_fp = true; // Float ops usually use FPR for rs2
  }

  // --- READ REGISTERS ---
  uint64_t reg1_value = rs1_is_fp ? registers_.ReadFpr(rs1) : registers_.ReadGpr(rs1);
  uint64_t reg2_value = rs2_is_fp ? registers_.ReadFpr(rs2) : registers_.ReadGpr(rs2);

  // --- CONTROL FLOW ---
  uint64_t branch_target = 0;
  bool is_branch_taken = false;
  bool is_control_flow = true; 

  if (opcode == 0b1101111) { // JAL
      is_branch_taken = true;
      branch_target = if_id_registers.pc + imm;
  } else if (opcode == 0b1100111) { // JALR
      is_branch_taken = true;
      branch_target = reg1_value + imm; 
  } else if (control_unit_.GetBranch()) { // Branches
      is_branch_taken = branch_alu.result(funct3, reg1_value, reg2_value);
      branch_target = if_id_registers.pc + imm;
  } else {
      is_control_flow = false; 
  }

    if (is_control_flow) {
      bool prediction_correct = true; 
      
      if (pipeline_mode == MODES::PIPELINE_H_F_STATIC_BRANCH) {
          prediction_correct = (is_branch_taken == false);
          std::cout << "[DECODE] (Mode 4) Branch. Taken=" << is_branch_taken << ". Predict=NOT_TAKEN. Correct=" << prediction_correct << std::endl;
      } else if (pipeline_mode >= MODES::PIPELINE_H_F_DYNAMIC_BRANCH) {
          prediction_correct = (is_branch_taken == if_id_registers.predicted_as_taken);
          std::cout << "[DECODE] (Mode 5) Branch. Taken=" << is_branch_taken << ". Predict=" << if_id_registers.predicted_as_taken << ". Correct=" << prediction_correct << std::endl;
          bpu_.update(if_id_registers.pc, is_branch_taken, branch_target);
      }
      
      if ((opcode == 0b1101111 || opcode == 0b1100111) && !if_id_registers.predicted_as_taken) {
          prediction_correct = false;
      }

      if (!prediction_correct) {
          std::cout << "[DECODE] MISPREDICT! Flushing." << std::endl;
          program_counter_ = is_branch_taken ? branch_target : if_id_registers.pc_plus_4;
          flush_fetch_ = true; 
          stall_count++;
          
          if (opcode == 0b1101111 || opcode == 0b1100111) {
              id_ex_reg_next = {}; 
              id_ex_reg_next.valid = true;
              id_ex_reg_next.signals.reg_write_ = true;
              id_ex_reg_next.alu_ans = if_id_registers.pc_plus_4; 
              id_ex_reg_next.rd = rd;
              id_ex_reg_next.instruction_bits = instruction;
          } else {
              id_ex_reg_next = {}; 
              id_ex_reg_next.valid = false;
          }
          return; 
      }
  }

  // 6. Fill ID/EX
  id_ex_reg_next.pc_plus_4 = if_id_registers.pc_plus_4;
  id_ex_reg_next.rs1_data = reg1_value;
  id_ex_reg_next.rs_2_data = reg2_value;
  id_ex_reg_next.imm = imm;
  id_ex_reg_next.rs1 = rs1;
  id_ex_reg_next.rs2 = rs2;
  id_ex_reg_next.rd = rd;
  id_ex_reg_next.funct3 = funct3; 
  id_ex_reg_next.funct7 = funct7; 
  id_ex_reg_next.instruction_bits = instruction;
  id_ex_reg_next.signals.alu_op = control_unit_.GetAluOp();
  id_ex_reg_next.signals.alu_src_ = control_unit_.GetAluSrc();
  id_ex_reg_next.signals.mem_read_ = control_unit_.GetMemRead();
  id_ex_reg_next.signals.mem_write_ = control_unit_.GetMemWrite();
  id_ex_reg_next.signals.reg_write_ = control_unit_.GetRegWrite();
  id_ex_reg_next.signals.mem_to_reg = control_unit_.GetMemToReg();
  id_ex_reg_next.forward_A = signals.forward_A;
  id_ex_reg_next.forward_B = signals.forward_B;
  id_ex_reg_next.valid = true;
}
void RVSSVM::pipeline_execute() {
  if (!id_ex_reg.valid) {
    std::cout << "[EXECUTE] Bubble" << std::endl;
    ex_mem_next = {};
    ex_mem_next.valid = false;
    return;
  }

  uint32_t inst = id_ex_reg.instruction_bits;
  uint8_t opcode = inst & 0b1111111;

  uint64_t reg1_value = id_ex_reg.rs1_data;
  uint64_t reg2_value = id_ex_reg.rs_2_data;

  switch (id_ex_reg.forward_A) {
      case ForwardSource::FROM_DECODE:
          reg1_value = id_ex_reg.rs1_data;
          break;
      case ForwardSource::FROM_EXECUTE:
          reg1_value = ex_mem.alu_ans;
          std::cout << "[EXECUTE] Forwarding EX->EX for rs1" << std::endl;
          break;
      case ForwardSource::FROM_MEMORY:
          reg1_value = mem_wb_reg.signals.mem_to_reg ? mem_wb_reg.mem_data : mem_wb_reg.alu_ans;
          std::cout << "[EXECUTE] Forwarding MEM->EX for rs1" << std::endl;
          break;
  }
  
  switch (id_ex_reg.forward_B) {
      case ForwardSource::FROM_DECODE:
          reg2_value = id_ex_reg.rs_2_data;
          break;
      case ForwardSource::FROM_EXECUTE:
          reg2_value = ex_mem.alu_ans;
          std::cout << "[EXECUTE] Forwarding EX->EX for rs2" << std::endl;
          break;
      case ForwardSource::FROM_MEMORY:
          reg2_value = mem_wb_reg.signals.mem_to_reg ? mem_wb_reg.mem_data : mem_wb_reg.alu_ans;
          std::cout << "[EXECUTE] Forwarding MEM->EX for rs2" << std::endl;
          break;
  }

  bool is_float = instruction_set::isFInstruction(inst);
  bool is_double = instruction_set::isDInstruction(inst);
  bool is_f_load_store = (opcode == 0b0000111 || opcode == 0b0100111); 

  if ((is_float || is_double) && !is_f_load_store) {
      if (is_float) {
          std::cout << "[EXECUTE] F instruction (stub)" << std::endl;
          pipeline_execute_float();
      } else {
          std::cout << "[EXECUTE] D instruction (stub)" << std::endl;
          pipeline_execute_double();
      }
      return;
  }

  if (opcode == 0b1110011) { 
      if ((inst >> 20) == 0) HandleSyscall();
      else {
          pipeline_execute_csr();
          std::cout << "[EXECUTE] CSR instruction (stub)" << std::endl;
      }
      return;
  }

  uint64_t alu_op2 = reg2_value;

  if (id_ex_reg.signals.alu_src_) {
    alu_op2 = static_cast<uint64_t>(id_ex_reg.imm);
  }

  uint64_t alu_result = 0;
  bool overflow;
  
  if (opcode == 0b1101111 || opcode == 0b1100111) {
      alu_result = id_ex_reg.pc_plus_4;
  } else {
      alu::AluOp aluOperation = control_unit_.GetAluSignal(inst, id_ex_reg.signals.alu_op);
      std::tie(alu_result, overflow) = alu_.execute(aluOperation, reg1_value, alu_op2);
  }

  std::cout << "[EXECUTE] OK. Result: 0x" << std::hex << alu_result << std::dec 
            << " (for reg x" << (int)id_ex_reg.rd << ")" << std::endl;

  ex_mem_next.alu_ans = alu_result;
  ex_mem_next.data = reg2_value;
  ex_mem_next.des_address = id_ex_reg.rd;
  ex_mem_next.funct3 = id_ex_reg.funct3;
  ex_mem_next.signals = id_ex_reg.signals;
  ex_mem_next.instruction_bits = inst;
  ex_mem_next.valid = true;
}

void RVSSVM::pipeline_mem() {
  if (!ex_mem.valid) {
    std::cout << "[MEMORY] Bubble" << std::endl;
    mem_wb_reg_next = {};
    mem_wb_reg_next.valid = false;
    return;
  }
  
  uint64_t addr = ex_mem.alu_ans; 
  uint8_t funct3 = ex_mem.funct3;
  uint64_t store_data = ex_mem.data;
  uint64_t mem_read_data = 0; 

  bool is_float = instruction_set::isFInstruction(ex_mem.instruction_bits);
  bool is_double = instruction_set::isDInstruction(ex_mem.instruction_bits);

  if (ex_mem.signals.mem_read_) {
    std::cout << "[MEMORY] OK. Reading from 0x" << std::hex << addr << std::dec << std::endl;

    if(is_float){
      mem_read_data = memory_controller_.ReadWord(addr);
    }
    else if(is_double){
      mem_read_data = memory_controller_.ReadDoubleWord(addr);
    }
    else{
      switch (funct3) {
        case 0b000: mem_read_data = static_cast<int8_t>(memory_controller_.ReadByte(addr)); break;
        case 0b001: mem_read_data = static_cast<int16_t>(memory_controller_.ReadHalfWord(addr)); break;
        case 0b010: mem_read_data = static_cast<int32_t>(memory_controller_.ReadWord(addr)); break;
        case 0b011: mem_read_data = memory_controller_.ReadDoubleWord(addr); break;
        case 0b100: mem_read_data = static_cast<uint8_t>(memory_controller_.ReadByte(addr)); break;
        case 0b101: mem_read_data = static_cast<uint16_t>(memory_controller_.ReadHalfWord(addr)); break;
        case 0b110: mem_read_data = static_cast<uint32_t>(memory_controller_.ReadWord(addr)); break;
        default: mem_read_data = memory_controller_.ReadDoubleWord(addr);
      }
    
    }

    
  }

  else if (ex_mem.signals.mem_write_) {
    std::cout << "[MEMORY] OK. Writing 0x" << std::hex << store_data << " to 0x" << addr << std::dec << std::endl;
    
    if (is_float) { // FSW
        memory_controller_.WriteWord(addr, store_data & 0xFFFFFFFF);
    } else if (is_double) { // FSD
        memory_controller_.WriteDoubleWord(addr, store_data);
    } else {
        // Integer Stores
        switch (funct3) {
            case 0b000: memory_controller_.WriteByte(addr, store_data & 0xFF); break;
            case 0b001: memory_controller_.WriteHalfWord(addr, store_data & 0xFFFF); break;
            case 0b010: memory_controller_.WriteWord(addr, store_data & 0xFFFFFFFF); break;
            case 0b011: memory_controller_.WriteDoubleWord(addr, store_data); break;
        }
    }
  }


  else {
    std::cout << "[MEMORY] OK. (No-op)" << std::endl;
  }


  
  
 
  
  mem_wb_reg_next.alu_ans = ex_mem.alu_ans;
  mem_wb_reg_next.mem_data = mem_read_data; 
  mem_wb_reg_next.des_address = ex_mem.des_address;
  mem_wb_reg_next.signals = ex_mem.signals;
  mem_wb_reg_next.instruction_bits = ex_mem.instruction_bits;
  mem_wb_reg_next.valid = true;
}

void RVSSVM::pipeline_write_back() {
  if (!mem_wb_reg.valid) {
    std::cout << "[WRITE_BACK] Bubble" << std::endl;
    return; 
  }

  if (mem_wb_reg.signals.reg_write_) {
    uint64_t data_to_write;
    uint8_t rd = mem_wb_reg.des_address;
    uint32_t inst = mem_wb_reg.instruction_bits;

    if (mem_wb_reg.signals.mem_to_reg) {
      data_to_write = mem_wb_reg.mem_data;
    } else {
      data_to_write = mem_wb_reg.alu_ans;
    }
    
    // --- TYPE DETECTION ---
    bool is_float = instruction_set::isFInstruction(inst);
    bool is_double = instruction_set::isDInstruction(inst);

    if (rd != 0 || is_float || is_double) { // FP regs can write to f0, only x0 is hardwired zero
      
        // Check if destination is GPR or FPR
        // Most F/D instructions write to FPR, but some (Compare, Class, Move-to-X) write to GPR.
        bool write_to_gpr = false;

        if (is_float) {
            uint8_t funct7 = (inst >> 25) & 0x7F;
            // f(eq|lt|le).s, fcvt.w.s, fmv.x.w, fclass.s write to Integer Reg
            if (funct7 == 0b1010000 || funct7 == 0b1100000 || funct7 == 0b1110000) {
                write_to_gpr = true;
            }
        }
        if (is_double) {
            uint8_t funct7 = (inst >> 25) & 0x7F;
            // f(eq|lt|le).d, fcvt.w.d, fmv.x.d, fclass.d write to Integer Reg
            if (funct7 == 0b1010001 || funct7 == 0b1100001 || funct7 == 0b1110001) {
                write_to_gpr = true;
            }
        }
        
        // Default Integer instructions always write to GPR
        if (!is_float && !is_double) write_to_gpr = true;


        if (write_to_gpr) {
            if (rd != 0) {
                std::cout << "[WRITE_BACK] Writing 0x" << std::hex << data_to_write << " to x" << std::dec << (int)rd << std::endl;
                registers_.WriteGpr(rd, data_to_write);
            }
        } else {
            // Write to FPR
            std::cout << "[WRITE_BACK] Writing 0x" << std::hex << data_to_write << " to f" << std::dec << (int)rd << std::endl;
            registers_.WriteFpr(rd, data_to_write);
        }

    } else {
      std::cout << "[WRITE_BACK] OK. (No-op, dest is x0)" << std::endl;
    }
    
    instructions_retired_++;
  } else {
    std::cout << "[WRITE_BACK] OK. (No-op, RegWrite=0)" << std::endl;
  }
}

// our Original Single-Cycle Functions as it is in repo
// (These are all UNCHANGED)

void RVSSVM::Fetch() {
  current_instruction_ = memory_controller_.ReadWord(program_counter_);
  UpdateProgramCounter(4);
}

void RVSSVM::Decode() {
  control_unit_.SetControlSignals(current_instruction_);
}

void RVSSVM::Execute() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;

  if (opcode == get_instr_encoding(Instruction::kecall).opcode && 
      funct3 == get_instr_encoding(Instruction::kecall).funct3) {
    HandleSyscall();
    return;
  }

  if (instruction_set::isFInstruction(current_instruction_)) { // RV64 F
    ExecuteFloat();
    return;
  } else if (instruction_set::isDInstruction(current_instruction_)) {
    ExecuteDouble();
    return;
  } else if (opcode==0b1110011) {
    ExecuteCsr();
    return;
  }

  uint8_t rs1 = (current_instruction_ >> 15) & 0b11111;
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;

  int32_t imm = ImmGenerator(current_instruction_);

  uint64_t reg1_value = registers_.ReadGpr(rs1);
  uint64_t reg2_value = registers_.ReadGpr(rs2);

  bool overflow = false;

  if (control_unit_.GetAluSrc()) {
    reg2_value = static_cast<uint64_t>(static_cast<int64_t>(imm));
  }

  alu::AluOp aluOperation = control_unit_.GetAluSignal(current_instruction_, control_unit_.GetAluOp());
  std::tie(execution_result_, overflow) = alu_.execute(aluOperation, reg1_value, reg2_value);


  if (control_unit_.GetBranch()) {
    if (opcode==get_instr_encoding(Instruction::kjalr).opcode || 
        opcode==get_instr_encoding(Instruction::kjal).opcode) {
      next_pc_ = static_cast<int64_t>(program_counter_); // PC was already updated in Fetch()
      UpdateProgramCounter(-4);
      return_address_ = program_counter_ + 4;
      if (opcode==get_instr_encoding(Instruction::kjalr).opcode) { 
        UpdateProgramCounter(-program_counter_ + (execution_result_));
      } else if (opcode==get_instr_encoding(Instruction::kjal).opcode) {
        UpdateProgramCounter(imm);
      }
    } else if (opcode==get_instr_encoding(Instruction::kbeq).opcode ||
               opcode==get_instr_encoding(Instruction::kbne).opcode ||
               opcode==get_instr_encoding(Instruction::kblt).opcode ||
               opcode==get_instr_encoding(Instruction::kbge).opcode ||
               opcode==get_instr_encoding(Instruction::kbltu).opcode ||
               opcode==get_instr_encoding(Instruction::kbgeu).opcode) {
      switch (funct3) {
        case 0b000: {// BEQ
          branch_flag_ = (execution_result_==0);
          break;
        }
        case 0b001: {// BNE
          branch_flag_ = (execution_result_!=0);
          break;
        }
        case 0b100: {// BLT
          branch_flag_ = (execution_result_==1);
          break;
        }
        case 0b101: {// BGE
          branch_flag_ = (execution_result_==0);
          break;
        }
        case 0b110: {// BLTU
          branch_flag_ = (execution_result_==1);
          break;
        }
        case 0b111: {// BGEU
          branch_flag_ = (execution_result_==0);
          break;
        }
      }
    }
  }

  
  if (branch_flag_ && opcode==0b1100011) {
    UpdateProgramCounter(-4);
    UpdateProgramCounter(imm);
  }


  if (opcode==get_instr_encoding(Instruction::kauipc).opcode) { // AUIPC
    execution_result_ = static_cast<int64_t>(program_counter_) - 4 + (imm << 12);

  }
}

void RVSSVM::ExecuteFloat() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;
  uint8_t funct7 = (current_instruction_ >> 25) & 0b1111111;
  uint8_t rm = funct3;
  uint8_t rs1 = (current_instruction_ >> 15) & 0b11111;
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;
  uint8_t rs3 = (current_instruction_ >> 27) & 0b11111;

  uint8_t fcsr_status = 0;

  int32_t imm = ImmGenerator(current_instruction_);

  if (rm==0b111) {
    rm = registers_.ReadCsr(0x002);
  }

  uint64_t reg1_value = registers_.ReadFpr(rs1);
  uint64_t reg2_value = registers_.ReadFpr(rs2);
  uint64_t reg3_value = registers_.ReadFpr(rs3);

  if (funct7==0b1101000 || funct7==0b1111000 || opcode==0b0000111 || opcode==0b0100111) {
    reg1_value = registers_.ReadGpr(rs1);
  }

  if (control_unit_.GetAluSrc()) {
    reg2_value = static_cast<uint64_t>(static_cast<int64_t>(imm));
  }

  alu::AluOp aluOperation = control_unit_.GetAluSignal(current_instruction_, control_unit_.GetAluOp());
  std::tie(execution_result_, fcsr_status) = alu::Alu::fpexecute(aluOperation, reg1_value, reg2_value, reg3_value, rm);

  registers_.WriteCsr(0x003, fcsr_status);
}

void RVSSVM::pipeline_execute_float() {
  uint32_t inst = id_ex_reg.instruction_bits;
  uint8_t funct3 = (inst >> 12) & 0x7;
  uint8_t rm = funct3;

  uint64_t val1 = id_ex_reg.rs1_data;
  uint64_t val2 = id_ex_reg.rs_2_data;

  // --- Forwarding A (Float) ---
  switch (id_ex_reg.forward_A) {
      case ForwardSource::FROM_DECODE:
          val1 = id_ex_reg.rs1_data;
          break;
      case ForwardSource::FROM_EXECUTE:
          val1 = ex_mem.alu_ans;
          std::cout << "[EXECUTE-F] Forwarding EX->EX for rs1" << std::endl;
          break;
      case ForwardSource::FROM_MEMORY:
          val1 = mem_wb_reg.signals.mem_to_reg ? mem_wb_reg.mem_data : mem_wb_reg.alu_ans;
          std::cout << "[EXECUTE-F] Forwarding MEM->EX for rs1" << std::endl;
          break;
  }

  // --- Forwarding B (Float) ---
  switch (id_ex_reg.forward_B) {
      case ForwardSource::FROM_DECODE:
          val2 = id_ex_reg.rs_2_data;
          break;
      case ForwardSource::FROM_EXECUTE:
          val2 = ex_mem.alu_ans;
          std::cout << "[EXECUTE-F] Forwarding EX->EX for rs2" << std::endl;
          break;
      case ForwardSource::FROM_MEMORY:
          val2 = mem_wb_reg.signals.mem_to_reg ? mem_wb_reg.mem_data : mem_wb_reg.alu_ans;
          std::cout << "[EXECUTE-F] Forwarding MEM->EX for rs2" << std::endl;
          break;
  }

  uint8_t rs3 = (inst >> 27) & 0x1F;
  uint64_t val3 = registers_.ReadFpr(rs3); 

  if (rm == 0b111) rm = registers_.ReadCsr(0x002);

  uint64_t result;
  uint8_t fcsr_status = 0;
  alu::AluOp op = control_unit_.GetAluSignal(inst, id_ex_reg.signals.alu_op);
  
  std::tie(result, fcsr_status) = alu::Alu::fpexecute(op, val1, val2, val3, rm);

  if (fcsr_status != 0) {
      uint64_t current_fcsr = registers_.ReadCsr(0x003);
      registers_.WriteCsr(0x003, current_fcsr | fcsr_status);
  }

  ex_mem_next.alu_ans = result;
  ex_mem_next.data = val2; 
  ex_mem_next.des_address = id_ex_reg.rd;
  ex_mem_next.funct3 = id_ex_reg.funct3;
  ex_mem_next.signals = id_ex_reg.signals;
  ex_mem_next.instruction_bits = id_ex_reg.instruction_bits;
  ex_mem_next.valid = true;
}

void RVSSVM::ExecuteDouble() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;
  uint8_t funct7 = (current_instruction_ >> 25) & 0b1111111;
  uint8_t rm = funct3;
  uint8_t rs1 = (current_instruction_ >> 15) & 0b11111;
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;
  uint8_t rs3 = (current_instruction_ >> 27) & 0b11111;

  uint8_t fcsr_status = 0;

  int32_t imm = ImmGenerator(current_instruction_);

  uint64_t reg1_value = registers_.ReadFpr(rs1);
  uint64_t reg2_value = registers_.ReadFpr(rs2);
  uint64_t reg3_value = registers_.ReadFpr(rs3);

  if (funct7==0b1101001 || funct7==0b1111001 || opcode==0b0000111 || opcode==0b0100111) {
    reg1_value = registers_.ReadGpr(rs1);
  }

  if (control_unit_.GetAluSrc()) {
    reg2_value = static_cast<uint64_t>(static_cast<int64_t>(imm));
  }

  alu::AluOp aluOperation = control_unit_.GetAluSignal(current_instruction_, control_unit_.GetAluOp());
  std::tie(execution_result_, fcsr_status) = alu::Alu::dfpexecute(aluOperation, reg1_value, reg2_value, reg3_value, rm);
}

void RVSSVM::pipeline_execute_double() {
  uint32_t inst = id_ex_reg.instruction_bits;
  uint8_t rm = (inst >> 12) & 0x7;

  uint64_t val1 = id_ex_reg.rs1_data;
  uint64_t val2 = id_ex_reg.rs_2_data;

  // --- Forwarding A ---
  switch (id_ex_reg.forward_A) {
      case ForwardSource::FROM_DECODE:  val1 = id_ex_reg.rs1_data; break;
      case ForwardSource::FROM_EXECUTE: 
          val1 = ex_mem.alu_ans;     
          std::cout << "[EXECUTE-D] Forwarding EX->EX for rs1" << std::endl;
          break;
      case ForwardSource::FROM_MEMORY:  
          val1 = mem_wb_reg.signals.mem_to_reg ? mem_wb_reg.mem_data : mem_wb_reg.alu_ans;
          std::cout << "[EXECUTE-D] Forwarding MEM->EX for rs1" << std::endl;
          break;
  }

  // --- Forwarding B ---
  switch (id_ex_reg.forward_B) {
      case ForwardSource::FROM_DECODE:  val2 = id_ex_reg.rs_2_data; break;
      case ForwardSource::FROM_EXECUTE: 
          val2 = ex_mem.alu_ans;     
          std::cout << "[EXECUTE-D] Forwarding EX->EX for rs2" << std::endl;
          break;
      case ForwardSource::FROM_MEMORY:  
          val2 = mem_wb_reg.signals.mem_to_reg ? mem_wb_reg.mem_data : mem_wb_reg.alu_ans;
          std::cout << "[EXECUTE-D] Forwarding MEM->EX for rs2" << std::endl;
          break;
  }

  uint8_t rs3 = (inst >> 27) & 0x1F;
  uint64_t val3 = registers_.ReadFpr(rs3); 

  if (rm == 0b111) rm = registers_.ReadCsr(0x002);

  uint64_t result;
  uint8_t fcsr_status = 0;
  alu::AluOp op = control_unit_.GetAluSignal(inst, id_ex_reg.signals.alu_op);

  std::tie(result, fcsr_status) = alu::Alu::dfpexecute(op, val1, val2, val3, rm);

  if (fcsr_status != 0) {
      uint64_t current_fcsr = registers_.ReadCsr(0x003);
      registers_.WriteCsr(0x003, current_fcsr | fcsr_status);
  }

  ex_mem_next.alu_ans = result;
  ex_mem_next.data = val2;
  ex_mem_next.des_address = id_ex_reg.rd;
  ex_mem_next.funct3 = id_ex_reg.funct3;
  ex_mem_next.signals = id_ex_reg.signals;
  ex_mem_next.instruction_bits = id_ex_reg.instruction_bits;
  ex_mem_next.valid = true;
}


void RVSSVM::ExecuteCsr() {
  uint8_t rs1 = (current_instruction_ >> 15) & 0b11111;
  uint16_t csr = (current_instruction_ >> 20) & 0xFFF;
  uint64_t csr_val = registers_.ReadCsr(csr);

  csr_target_address_ = csr;
  csr_old_value_ = csr_val;
  csr_write_val_ = registers_.ReadGpr(rs1);
  csr_uimm_ = rs1;
}


void RVSSVM::pipeline_execute_csr() {
  uint32_t inst = id_ex_reg.instruction_bits;
  uint8_t rs1 = (inst >> 15) & 0x1F;
  uint16_t csr_addr = (inst >> 20) & 0xFFF;
  uint8_t funct3 = (inst >> 12) & 0x7;

  // Read current CSR value
  uint64_t old_val = registers_.ReadCsr(csr_addr);
  uint64_t write_val = 0;
  uint64_t uimm = rs1; // For immediate CSR ops
  uint64_t rs1_val = id_ex_reg.rs1_data;

  // Calculate new value based on funct3
  // CSRRW (1), CSRRS (2), CSRRC (3), CSRRWI (5), CSRRSI (6), CSRRCI (7)
  switch (funct3) {
      case 0b001: // CSRRW
          write_val = rs1_val;
          break;
      case 0b010: // CSRRS (Read and Set Bit)
          write_val = old_val | rs1_val;
          break;
      case 0b011: // CSRRC (Read and Clear Bit)
          write_val = old_val & ~rs1_val;
          break;
      case 0b101: // CSRRWI
          write_val = uimm;
          break;
      case 0b110: // CSRRSI
          write_val = old_val | uimm;
          break;
      case 0b111: // CSRRCI
          write_val = old_val & ~uimm;
          break;
      default:
          write_val = old_val; // Should not happen for valid CSR ops
          break;
  }

  // Perform the Write (Effectively commit change now for simplicity)
  // In a precise pipeline, this might happen in WB, but CSR effects are often immediate in sims.
  // We check if we should write (rs1 != 0 for Set/Clear)
  bool perform_write = true;
  if ((funct3 == 0b010 || funct3 == 0b011 || funct3 == 0b110 || funct3 == 0b111) && rs1 == 0) {
      perform_write = false; 
  }

  if (perform_write) {
      registers_.WriteCsr(csr_addr, write_val);
  }

  // The result of the instruction is the OLD value of the CSR (written to rd)
  ex_mem_next.alu_ans = old_val; 
}

void RVSSVM::HandleSyscall() {
  uint64_t syscall_number = registers_.ReadGpr(17);
  switch (syscall_number) {
    case SYSCALL_PRINT_INT: {
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        } else {
          std::cout << "VM_STDOUT_START";
        }
        std::cout << static_cast<int64_t>(registers_.ReadGpr(10)); // Print signed integer
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        } else {
          std::cout << "VM_STDOUT_END" << std::endl;
        }
        break;
    }
    case SYSCALL_PRINT_FLOAT: { // print float
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        } else {
          std::cout << "VM_STDOUT_START";
        }
        float float_value;
        uint64_t raw = registers_.ReadGpr(10);
        std::memcpy(&float_value, &raw, sizeof(float_value));
        std::cout << std::setprecision(std::numeric_limits<float>::max_digits10) << float_value;
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        } else {
          std::cout << "VM_STDOUT_END" << std::endl;
        }
        break;
    }
    case SYSCALL_PRINT_DOUBLE: { // print double
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        } else {
          std::cout << "VM_STDOUT_START";
        }
        double double_value;
        uint64_t raw = registers_.ReadGpr(10);
        std::memcpy(&double_value, &raw, sizeof(double_value));
        std::cout << std::setprecision(std::numeric_limits<double>::max_digits10) << double_value;
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        } else {
          std::cout << "VM_STDOUT_END" << std::endl;
        }
        break;
    }
    case SYSCALL_PRINT_STRING: {
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        }
        PrintString(registers_.ReadGpr(10)); // Print string
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        }
        break;
    }
    case SYSCALL_EXIT: {
        stop_requested_ = true; // Stop the VM
        if (!globals::vm_as_backend) {
            std::cout << "VM_EXIT" << std::endl;
        }
        output_status_ = "VM_EXIT";
        std::cout << "Exited with exit code: " << registers_.ReadGpr(10) << std::endl;
        exit(0); // Exit the program
        break;
    }
    case SYSCALL_READ: { // Read
      uint64_t file_descriptor = registers_.ReadGpr(10);
      uint64_t buffer_address = registers_.ReadGpr(11);
      uint64_t length = registers_.ReadGpr(12);

      if (file_descriptor == 0) {
        // Read from stdin
        std::string input;
        {
          std::cout << "VM_STDIN_START" << std::endl;
          output_status_ = "VM_STDIN_START";
          std::unique_lock<std::mutex> lock(input_mutex_);
          input_cv_.wait(lock, [this]() { 
            return !input_queue_.empty(); 
          });
          output_status_ = "VM_STDIN_END";
          std::cout << "VM_STDIN_END" << std::endl;

          input = input_queue_.front();
          input_queue_.pop();
        }


        std::vector<uint8_t> old_bytes_vec(length, 0);
        std::vector<uint8_t> new_bytes_vec(length, 0);

        for (size_t i = 0; i < length; ++i) {
          old_bytes_vec[i] = memory_controller_.ReadByte(buffer_address + i);
        }
        
        for (size_t i = 0; i < input.size() && i < length; ++i) {
          memory_controller_.WriteByte(buffer_address + i, static_cast<uint8_t>(input[i]));
        }
        if (input.size() < length) {
          memory_controller_.WriteByte(buffer_address + input.size(), '\0');
        }

        for (size_t i = 0; i < length; ++i) {
          new_bytes_vec[i] = memory_controller_.ReadByte(buffer_address + i);
        }

        current_delta_.memory_changes.push_back({
          buffer_address, 
          old_bytes_vec, 
          new_bytes_vec
        });

        uint64_t old_reg = registers_.ReadGpr(10);
        unsigned int reg_index = 10;
        unsigned int reg_type = 0; // 0 for GPR, 1 for CSR, 2 for FPR
        uint64_t new_reg = std::min(static_cast<uint64_t>(length), static_cast<uint64_t>(input.size()));
        registers_.WriteGpr(10, new_reg); 
        if (old_reg != new_reg) {
          current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
        }

      } else {
          std::cerr << "Unsupported file descriptor: " << file_descriptor << std::endl;
      }
      break;
    }
    case SYSCALL_WRITE: { // Write
        uint64_t file_descriptor = registers_.ReadGpr(10);
        uint64_t buffer_address = registers_.ReadGpr(11);
        uint64_t length = registers_.ReadGpr(12);

        if (file_descriptor == 1) { // stdout
          std::cout << "VM_STDOUT_START";
          output_status_ = "VM_STDOUT_START";
          uint64_t bytes_printed = 0;
          for (uint64_t i = 0; i < length; ++i) {
              char c = memory_controller_.ReadByte(buffer_address + i);
              // if (c == '\0') {
              //     break;
              // }
              std::cout << c;
              bytes_printed++;
          }
          std::cout << std::flush; 
          output_status_ = "VM_STDOUT_END";
          std::cout << "VM_STDOUT_END" << std::endl;

          uint64_t old_reg = registers_.ReadGpr(10);
          unsigned int reg_index = 10;
          unsigned int reg_type = 0; // 0 for GPR, 1 for CSR, 2 for FPR
          uint64_t new_reg = std::min(static_cast<uint64_t>(length), bytes_printed);
          registers_.WriteGpr(10, new_reg);
          if (old_reg != new_reg) {
            current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
          }
        } else {
            std::cerr << "Unsupported file descriptor: " << file_descriptor << std::endl;
        }
        break;
    }
    default: {
      std::cerr << "Unknown syscall number: " << syscall_number << std::endl;
      break;
    }
  }
}

void RVSSVM::WriteMemory() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;

  if (opcode == 0b1110011 && funct3 == 0b000) {
    return;
  }

  if (instruction_set::isFInstruction(current_instruction_)) { // RV64 F
    WriteMemoryFloat();
    return;
  } else if (instruction_set::isDInstruction(current_instruction_)) {
    WriteMemoryDouble();
    return;
  }

  if (control_unit_.GetMemRead()) {
    switch (funct3) {
      case 0b000: {// LB
        memory_result_ = static_cast<int8_t>(memory_controller_.ReadByte(execution_result_));
        break;
      }
      case 0b001: {// LH
        memory_result_ = static_cast<int16_t>(memory_controller_.ReadHalfWord(execution_result_));
        break;
      }
      case 0b010: {// LW
        memory_result_ = static_cast<int32_t>(memory_controller_.ReadWord(execution_result_));
        break;
      }
      case 0b011: {// LD
        memory_result_ = memory_controller_.ReadDoubleWord(execution_result_);
        break;
      }
      case 0b100: {// LBU
        memory_result_ = static_cast<uint8_t>(memory_controller_.ReadByte(execution_result_));
        break;
      }
      case 0b101: {// LHU
        memory_result_ = static_cast<uint16_t>(memory_controller_.ReadHalfWord(execution_result_));
        break;
      }
      case 0b110: {// LWU
        memory_result_ = static_cast<uint32_t>(memory_controller_.ReadWord(execution_result_));
        break;
      }
    }
  }

  uint64_t addr = 0;
  std::vector<uint8_t> old_bytes_vec;
  std::vector<uint8_t> new_bytes_vec;

  if (control_unit_.GetMemWrite()) {
    switch (funct3) {
      case 0b000: {// SB
        addr = execution_result_;
        old_bytes_vec.push_back(memory_controller_.ReadByte(addr));
        memory_controller_.WriteByte(execution_result_, registers_.ReadGpr(rs2) & 0xFF);
        new_bytes_vec.push_back(memory_controller_.ReadByte(addr));
        break;
      }
      case 0b001: {// SH
        addr = execution_result_;
        for (size_t i = 0; i < 2; ++i) {
          old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        memory_controller_.WriteHalfWord(execution_result_, registers_.ReadGpr(rs2) & 0xFFFF);
        for (size_t i = 0; i < 2; ++i) {
          new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        break;
      }
      case 0b010: {// SW
        addr = execution_result_;
        for (size_t i = 0; i < 4; ++i) {
          old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        memory_controller_.WriteWord(execution_result_, registers_.ReadGpr(rs2) & 0xFFFFFFFF);
        for (size_t i = 0; i < 4; ++i) {
          new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        break;
      }
      case 0b011: {// SD
        addr = execution_result_;
        for (size_t i = 0; i < 8; ++i) {
          old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        memory_controller_.WriteDoubleWord(execution_result_, registers_.ReadGpr(rs2) & 0xFFFFFFFFFFFFFFFF);
        for (size_t i = 0; i < 8; ++i) {
          new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        break;
      }
    }
  }

  if (old_bytes_vec != new_bytes_vec) {
    current_delta_.memory_changes.push_back({
      addr,
      old_bytes_vec,
      new_bytes_vec
    });
  }
}

void RVSSVM::WriteMemoryFloat() {
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;

  if (control_unit_.GetMemRead()) { // FLW
    memory_result_ = memory_controller_.ReadWord(execution_result_);
  }

  uint64_t addr = 0;
  std::vector<uint8_t> old_bytes_vec;
  std::vector<uint8_t> new_bytes_vec;

  if (control_unit_.GetMemWrite()) { // FSW
    addr = execution_result_;
    for (size_t i = 0; i < 4; ++i) {
      old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
    uint32_t val = registers_.ReadFpr(rs2) & 0xFFFFFFFF;
    memory_controller_.WriteWord(execution_result_, val);
    for (size_t i = 0; i < 4; ++i) {
      new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
  }

  if (old_bytes_vec!=new_bytes_vec) {
    current_delta_.memory_changes.push_back({addr, old_bytes_vec, new_bytes_vec});
  }
}

void RVSSVM::WriteMemoryDouble() {
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;

  if (control_unit_.GetMemRead()) {// FLD
    memory_result_ = memory_controller_.ReadDoubleWord(execution_result_);
  }

  uint64_t addr = 0;
  std::vector<uint8_t> old_bytes_vec;
  std::vector<uint8_t> new_bytes_vec;

  if (control_unit_.GetMemWrite()) {// FSD
    addr = execution_result_;
    for (size_t i = 0; i < 8; ++i) {
      old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
    memory_controller_.WriteDoubleWord(execution_result_, registers_.ReadFpr(rs2));
    for (size_t i = 0; i < 8; ++i) {
      new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
  }

  if (old_bytes_vec!=new_bytes_vec) {
    current_delta_.memory_changes.push_back({addr, old_bytes_vec, new_bytes_vec});
  }
}

void RVSSVM::WriteBack() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;
  uint8_t rd = (current_instruction_ >> 7) & 0b11111;
  int32_t imm = ImmGenerator(current_instruction_);

  if (opcode == get_instr_encoding(Instruction::kecall).opcode && 
      funct3 == get_instr_encoding(Instruction::kecall).funct3) { // ecall
    return;
  }

  if (instruction_set::isFInstruction(current_instruction_)) { // RV64 F
    WriteBackFloat();
    return;
  } else if (instruction_set::isDInstruction(current_instruction_)) {
    WriteBackDouble();
    return;
  } else if (opcode==0b1110011) { // CSR opcode
    WriteBackCsr();
    return;
  }

  uint64_t old_reg = registers_.ReadGpr(rd);
  unsigned int reg_index = rd;
  unsigned int reg_type = 0; // 0 for GPR, 1 for CSR, 2 for FPR


  if (control_unit_.GetRegWrite()) { 
    switch (opcode) {
      case get_instr_encoding(Instruction::kRtype).opcode: /* R-Type */
      case get_instr_encoding(Instruction::kItype).opcode: /* I-Type */
      case get_instr_encoding(Instruction::kauipc).opcode: /* AUIPC */ {
        registers_.WriteGpr(rd, execution_result_);
        break;
      }
      case get_instr_encoding(Instruction::kLoadType).opcode: /* Load */ { 
        registers_.WriteGpr(rd, memory_result_);
        break;
      }
      case get_instr_encoding(Instruction::kjalr).opcode: /* JALR */
      case get_instr_encoding(Instruction::kjal).opcode: /* JAL */ {
        registers_.WriteGpr(rd, next_pc_);
        break;
      }
      case get_instr_encoding(Instruction::klui).opcode: /* LUI */ {
        registers_.WriteGpr(rd, (imm << 12));
        break;
      }
      default: break;
    }
  }

  if (opcode==get_instr_encoding(Instruction::kjal).opcode) /* JAL */ {
    // Updated in Execute()
  }
  if (opcode==get_instr_encoding(Instruction::kjalr).opcode) /* JALR */ {
    // registers_.WriteGpr(rd, return_address_); // Write back to rs1
    // Updated in Execute()
  }

  uint64_t new_reg = registers_.ReadGpr(rd);
  if (old_reg!=new_reg) {
    current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
  }
}

void RVSSVM::WriteBackFloat() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct7 = (current_instruction_ >> 25) & 0b1111111;
  uint8_t rd = (current_instruction_ >> 7) & 0b11111;

  uint64_t old_reg = 0;
  unsigned int reg_index = rd;
  unsigned int reg_type = 2; // 0 for GPR, 1 for CSR, 2 for FPR
  uint64_t new_reg = 0;

  if (control_unit_.GetRegWrite()) {
    switch(funct7) {
      // write to GPR
      case get_instr_encoding(Instruction::kfle_s).funct7: // f(eq|lt|le).s
      case get_instr_encoding(Instruction::kfcvt_w_s).funct7: // fcvt.(w|wu|l|lu).s
      case get_instr_encoding(Instruction::kfmv_x_w).funct7: // fmv.x.w , fclass.s
      {
        old_reg = registers_.ReadGpr(rd);
        registers_.WriteGpr(rd, execution_result_);
        new_reg = execution_result_;
        reg_type = 0; // GPR
        break;
      }

      // write to FPR
      default: {
        switch (opcode) {
          case get_instr_encoding(Instruction::kflw).opcode: {
            old_reg = registers_.ReadFpr(rd);
            registers_.WriteFpr(rd, memory_result_);
            new_reg = memory_result_;
            reg_type = 2; // FPR
            break;
          }

          default: {
            old_reg = registers_.ReadFpr(rd);
            registers_.WriteFpr(rd, execution_result_);
            new_reg = execution_result_;
            reg_type = 2; // FPR
            break;
          }
        }
      }
    }
  }

  if (old_reg!=new_reg) {
    current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
  }
}

void RVSSVM::WriteBackDouble() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct7 = (current_instruction_ >> 25) & 0b1111111;
  uint8_t rd = (current_instruction_ >> 7) & 0b11111;

  uint64_t old_reg = 0;
  unsigned int reg_index = rd;
  unsigned int reg_type = 2; // 0 for GPR, 1 for CSR, 2 for FPR
  uint64_t new_reg = 0;

  if (control_unit_.GetRegWrite()) {
    // write to GPR
    if (funct7==0b1010001
        || funct7==0b1100001
        || funct7==0b1110001) { // f(eq|lt|le).d, fcvt.(w|wu|l|lu).d
      old_reg = registers_.ReadGpr(rd);
      registers_.WriteGpr(rd, execution_result_);
      new_reg = execution_result_;
      reg_type = 0; // GPR
    }
      // write to FPR
    else if (opcode==0b0000111) {
      old_reg = registers_.ReadFpr(rd);
      registers_.WriteFpr(rd, memory_result_);
      new_reg = memory_result_;
      reg_type = 2; // FPR
    } else {
      old_reg = registers_.ReadFpr(rd);
      registers_.WriteFpr(rd, execution_result_);
      new_reg = execution_result_;
      reg_type = 2; // FPR
    }
  }

  if (old_reg!=new_reg) {
    current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
  }

  return;
}

void RVSSVM::WriteBackCsr() {
  uint8_t rd = (current_instruction_ >> 7) & 0b11111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;

  switch (funct3) {
    case get_instr_encoding(Instruction::kcsrrw).funct3: { // CSRRW
      registers_.WriteGpr(rd, csr_old_value_);
      registers_.WriteCsr(csr_target_address_, csr_write_val_);
      break;
    }
    case get_instr_encoding(Instruction::kcsrrs).funct3: { // CSRRS
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_write_val_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ | csr_write_val_);
      }
      break;
    }
    case get_instr_encoding(Instruction::kcsrrc).funct3: { // CSRRC
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_write_val_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ & ~csr_write_val_);
      }
      break;
    }
    case get_instr_encoding(Instruction::kcsrrwi).funct3: { // CSRRWI
      registers_.WriteGpr(rd, csr_old_value_);
      registers_.WriteCsr(csr_target_address_, csr_uimm_);
      break;
    }
    case get_instr_encoding(Instruction::kcsrrsi).funct3: { // CSRRSI
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_uimm_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ | csr_uimm_);
      }
      break;
    }
    case get_instr_encoding(Instruction::kcsrrci).funct3: { // CSRRCI
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_uimm_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ & ~csr_uimm_);
      }
      break;
    }
  }
}


void RVSSVM::Run_single(){
    ClearStop();
    uint64_t instruction_executed = 0;
    running_ = true; 

    while (!stop_requested_ && program_counter_ < program_size_) {
      if (instruction_executed > vm_config::config.getInstructionExecutionLimit())
        break;

      Fetch();
      Decode();
      Execute();
      WriteMemory();
      WriteBack();
      instructions_retired_++;
      instruction_executed++;
      cycle_s_++;
    }
    running_ = false; 
    if (program_counter_ >= program_size_) {
      std::cout << "VM_PROGRAM_END" << std::endl;
      output_status_ = "VM_PROGRAM_END";
    }
    DumpRegisters(globals::registers_dump_file_path, registers_);
    DumpState(globals::vm_state_dump_file_path);
}


void RVSSVM::Run() {
  int get_mode = vm_config::config.getPipelineMode();
  pipeline_mode = static_cast<MODES>(get_mode);
  
  std::cout << "--- Starting VM Run ---" << std::endl;
  if (pipeline_mode == MODES::SINGLE_CYLCE) {
    std::cout << "Mode 0: SINGLE_CYLCE" << std::endl;
    Run_single();
  } else {
    if(pipeline_mode == MODES::PIPELINE_NO_HAZARD) std::cout << "Mode 1: PIPELINE_NO_HAZARD (Expect wrong results for hazards)" << std::endl;
    else if(pipeline_mode == MODES::PIPELINE_HAZARD) std::cout << "Mode 2: PIPELINE_HAZARD (Stall-Only)" << std::endl;
    else if(pipeline_mode == MODES::PIPELINE_HAZARD_FORWARDING) std::cout << "Mode 3: PIPELINE_HAZARD_FORWARDING" << std::endl;
    
    Run_Pipelined();
  }
}

void RVSSVM::DebugRun() {
  int get_mode = vm_config::config.getPipelineMode();
  pipeline_mode = static_cast<MODES>(get_mode);
    
  if (pipeline_mode != MODES::SINGLE_CYLCE) {
    std::cout << "DebugRun is only supported in Single-Cycle mode." << std::endl;
    return;
  }
  
  ClearStop();
  uint64_t instruction_executed = 0;
  running_ = true; 
  while (!stop_requested_ && program_counter_ < program_size_) {
    if (instruction_executed > vm_config::config.getInstructionExecutionLimit())
      break;
    
    if (std::find(breakpoints_.begin(), breakpoints_.end(), program_counter_) == breakpoints_.end()) {
      Step_single(); 
      instruction_executed++; 
      
      unsigned int delay_ms = vm_config::config.getRunStepDelay();
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      
    } else {
      std::cout << "VM_BREAKPOINT_HIT " << program_counter_ << std::endl;
      output_status_ = "VM_BREAKPOINT_HIT";
      break;
    }
  }
  running_ = false; 
  if (program_counter_ >= program_size_) {
    std::cout << "VM_PROGRAM_END" << std::endl;
    output_status_ = "VM_PROGRAM_END";
  }
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

void RVSSVM::Step_single(){
  current_delta_.old_pc = program_counter_;
  if (program_counter_ < program_size_) {
    Fetch();
    Decode();
    Execute();
    WriteMemory();
    WriteBack();
    instructions_retired_++;
    cycle_s_++;
    std::cout << "Program Counter: " << std::hex << program_counter_ << std::dec << std::endl;

    current_delta_.new_pc = program_counter_;

    undo_stack_.push(current_delta_);
    while (!redo_stack_.empty()) {
      redo_stack_.pop();
    }

    current_delta_ = StepDelta();


    if (program_counter_ < program_size_) {
      std::cout << "VM_STEP_COMPLETED" << std::endl;
      output_status_ = "VM_STEP_COMPLETED";
    } else if (program_counter_ >= program_size_) {
      std::cout << "VM_LAST_INSTRUCTION_STEPPED" << std::endl;
      output_status_ = "VM_LAST_INSTRUCTION_STEPPED";
    }

  } else if (program_counter_ >= program_size_) {
    std::cout << "VM_PROGRAM_END" << std::endl;
    output_status_ = "VM_PROGRAM_END";
  }
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

void RVSSVM::Step() {
  int get_mode = vm_config::config.getPipelineMode(); 
  pipeline_mode = static_cast<MODES>(get_mode);

  if (pipeline_mode == MODES::SINGLE_CYLCE) {
    Step_single();
  } else {
    if (running_ || stop_requested_) return; 

    // Check if pipeline is *fully* empty before stopping
    bool pipeline_empty = !if_id_registers.valid && !id_ex_reg.valid && !ex_mem.valid && !mem_wb_reg.valid;
    
    if (program_counter_ < program_size_ || !pipeline_empty) {
        Clocktick();
        std::cout << "Clock cycle advanced. Fetch PC: " << std::hex << program_counter_ << std::dec << std::endl;
        output_status_ = "VM_STEP_COMPLETED";
    } else {
        std::cout << "VM_PROGRAM_END" << std::endl;
        output_status_ = "VM_PROGRAM_END";
    }
    DumpRegisters(globals::registers_dump_file_path, registers_);
    DumpState(globals::vm_state_dump_file_path);
  }
}

void RVSSVM::Undo() {
  if (undo_stack_.empty()) {
    std::cout << "VM_NO_MORE_UNDO" << std::endl;
    output_status_ = "VM_NO_MORE_UNDO";
    return;
  }

  StepDelta last = undo_stack_.top();
  undo_stack_.pop();

  for (const auto &change : last.register_changes) {
    switch (change.reg_type) {
      case 0: { // GPR
        registers_.WriteGpr(change.reg_index, change.old_value);
        break;
      }
      case 1: { // CSR
        registers_.WriteCsr(change.reg_index, change.old_value);
        break;
      }
      case 2: { // FPR
        registers_.WriteFpr(change.reg_index, change.old_value);
        break;
      }
      default:std::cerr << "Install register type: " << change.reg_type << std::endl;
        break;
    }
  }

  for (const auto &change : last.memory_changes) {
    for (size_t i = 0; i < change.old_bytes_vec.size(); ++i) {
      memory_controller_.WriteByte(change.address + i, change.old_bytes_vec[i]);
    }
  }

  program_counter_ = last.old_pc;
  instructions_retired_--;
  cycle_s_--;
  std::cout << "Program Counter: " << program_counter_ << std::endl;

  redo_stack_.push(last);

  output_status_ = "VM_UNDO_COMPLETED";
  std::cout << "VM_UNDO_COMPLETED" << std::endl;

  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

void RVSSVM::Redo() {
  if (redo_stack_.empty()) {
    std::cout << "VM_NO_MORE_REDO" << std::endl;
    return;
  }

  StepDelta next = redo_stack_.top();
  redo_stack_.pop();

  for (const auto &change : next.register_changes) {
    switch (change.reg_type) {
      case 0: { // GPR
        registers_.WriteGpr(change.reg_index, change.new_value);
        break;
      }
      case 1: { // CSR
        registers_.WriteCsr(change.reg_index, change.new_value);
        break;
      }
      case 2: { // FPR
        registers_.WriteFpr(change.reg_index, change.new_value);
        break;
      }
      default:std::cerr << "Invalid register type: " << change.reg_type << std::endl;
        break;
    }
  }

  for (const auto &change : next.memory_changes) {
    for (size_t i = 0; i < change.new_bytes_vec.size(); ++i) {
      memory_controller_.WriteByte(change.address + i, change.new_bytes_vec[i]);
    }
  }

  program_counter_ = next.new_pc;
  instructions_retired_++;
  cycle_s_++;
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
  std::cout << "Program Counter: " << program_counter_ << std::endl;
  undo_stack_.push(next);

}

void RVSSVM::Reset() {
  program_counter_ = 0;
  instructions_retired_ = 0;
  cycle_s_ = 0;
  registers_.Reset();
  memory_controller_.Reset();
  control_unit_.Reset();
  branch_flag_ = false;
  next_pc_ = 0;
  execution_result_ = 0;
  memory_result_ = 0;

  return_address_ = 0;
  csr_target_address_ = 0;
  csr_old_value_ = 0;
  csr_write_val_ = 0;
  csr_uimm_ = 0;
  current_delta_.register_changes.clear();
  current_delta_.memory_changes.clear();
  current_delta_.old_pc = 0;
  current_delta_.new_pc = 0;
  undo_stack_ = std::stack<StepDelta>();
  redo_stack_ = std::stack<StepDelta>();


  stall_count = 0;
  if_id_registers = {};
  id_ex_reg = {};
  ex_mem = {};
  mem_wb_reg = {};
  pipeline_stalled_ = false;

  bpu_.reset();
  
}

void RVSSVM::Run_Pipelined() {
  ClearStop();
  running_ = true;

  // --- Main fetch loop ---
  while (running_ && !stop_requested_ && program_counter_ < program_size_) {
    if (instructions_retired_ > vm_config::config.getInstructionExecutionLimit())
      break;
    Clocktick(); // Runs one full pipeline cycle
    // std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // --- Pipeline flush loop ---
  int flush_cycles = 4; // k-1 stages
  while (running_ && !stop_requested_ && flush_cycles >= 0) {
      Clocktick();
      flush_cycles--;
      // std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  running_ = false;
  if (program_counter_ >= program_size_ && !stop_requested_) {
    std::cout << "VM_PROGRAM_END" << std::endl;
    output_status_ = "VM_PROGRAM_END";
  }
  
  // --- ADDED STATS PRINTING ---
  std::cout << "--- Pipeline Run Stats ---" << std::endl;
  std::cout << "Total Cycles: " << std::dec << cycle_s_ << std::endl;
  std::cout << "Total Stalls: " << std::dec << stall_count << std::endl;
  

  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}