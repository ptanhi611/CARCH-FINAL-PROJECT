#include "vm/hazard_detection_unit.h"
#include "common/instructions.h" 
#include <iostream>
#include <iomanip> // For hex printing

using instruction_set::Instruction;
using instruction_set::get_instr_encoding;

PipelineControlSignals HazardDetectionUnit::compute_signals(
    int mode,
    uint32_t if_id_inst,
    const ID_EX_registers& id_ex_reg,
    const EX_MEM_registers& ex_mem,
    const MEM_WB_registers& mem_wb_reg) 
{
    PipelineControlSignals signals;
    if (mode == 1) return signals;

    // --- 1. DECODE INSTRUCTION ---
    uint8_t opcode = if_id_inst & 0x7F;
    uint8_t rs1 = (if_id_inst >> 15) & 0b11111;
    uint8_t rs2 = (if_id_inst >> 20) & 0b11111;
    uint8_t rs3 = (if_id_inst >> 27) & 0b11111;

    // Explicitly identify ADDI (0x13) and LD (0x03)
    bool is_float = instruction_set::isFInstruction(if_id_inst) || instruction_set::isDInstruction(if_id_inst);

    // Default Assumption: Use operands unless proven otherwise
    bool use_rs1 = false;
    bool use_rs2 = false;
    bool use_rs3 = false;

    if (is_float) {
        if (opcode == 0x07) use_rs1 = true; // FLW
        else if (opcode == 0x27) { use_rs1 = true; use_rs2 = true; } // FSW
        else if (opcode == 0x53) { use_rs1 = true; use_rs2 = true; } // F-ALU
        else if ((opcode & 0xCF) == 0x43) { use_rs1 = true; use_rs2 = true; use_rs3 = true; }
    } else {
        // Integer
        // RS1 used by: I-Type (0x13, 0x03), R-Type (0x33, 0x3B), Store (0x23), Branch (0x63), JALR (0x67)
        if (opcode == 0x13 || opcode == 0x03 || opcode == 0x1B || opcode == 0x33 || opcode == 0x3B || 
            opcode == 0x23 || opcode == 0x63 || opcode == 0x67) {
            use_rs1 = true;
        }
        // RS2 used by: R-Type, Store, Branch
        if (opcode == 0x33 || opcode == 0x3B || opcode == 0x23 || opcode == 0x63) {
            use_rs2 = true;
        }
    }

    // ========================================================
    // MODE 2: STALL-ONLY
    // ========================================================
    if (mode == 2) {
        bool stall = false;
        // Check EX
        if (id_ex_reg.valid && id_ex_reg.signals.reg_write_ && id_ex_reg.rd != 0) {
            if ((use_rs1 && id_ex_reg.rd == rs1) || (use_rs2 && id_ex_reg.rd == rs2) || (use_rs3 && id_ex_reg.rd == rs3)) stall = true;
        }
        // Check MEM
        if (ex_mem.valid && ex_mem.signals.reg_write_ && ex_mem.des_address != 0) {
            if ((use_rs1 && ex_mem.des_address == rs1) || (use_rs2 && ex_mem.des_address == rs2) || (use_rs3 && ex_mem.des_address == rs3)) stall = true;
        }

        if (stall) {
            // std::cout << "[HDU] STALL (Mode 2)" << std::endl;
            signals.stall_fetch = true;
            signals.flush_decode = true;
            return signals;
        }
    }

    // ========================================================
    // MODE 3+: FORWARDING
    // ========================================================
    if (mode >= 3) {

        // 1. LOAD-USE HAZARD (Stall)
        if (id_ex_reg.valid && id_ex_reg.signals.mem_read_ && id_ex_reg.rd != 0) {
            if ((use_rs1 && id_ex_reg.rd == rs1) || (use_rs2 && id_ex_reg.rd == rs2) || (use_rs3 && id_ex_reg.rd == rs3)) {
                std::cout << "[HDU] STALL (Mode 3): Load-Use Hazard" << std::endl;
                signals.stall_fetch = true;
                signals.flush_decode = true;
                return signals;
            }
        }

        // 2. CONTROL HAZARD (Stall)
        if (opcode == 0x63 || opcode == 0x67) {
            bool control_stall = false;
            if (id_ex_reg.valid && id_ex_reg.signals.reg_write_ && id_ex_reg.rd != 0) {
                if ((use_rs1 && id_ex_reg.rd == rs1) || (use_rs2 && id_ex_reg.rd == rs2)) control_stall = true;
            }
            if (ex_mem.valid && ex_mem.signals.reg_write_ && ex_mem.des_address != 0) {
                if ((use_rs1 && ex_mem.des_address == rs1) || (use_rs2 && ex_mem.des_address == rs2)) control_stall = true;
            }
            if (control_stall) {
                std::cout << "[HDU] STALL (Mode 3): Control Dependency" << std::endl;
                signals.stall_fetch = true;
                signals.flush_decode = true;
                return signals;
            }
        }

        // 3. FORWARDING LOGIC
        
        // --- Forward A (RS1) ---
        if (use_rs1) {
            // Priority 1: EX Stage
            if (id_ex_reg.valid && id_ex_reg.signals.reg_write_ && id_ex_reg.rd != 0 && id_ex_reg.rd == rs1) {
                signals.forward_A = ForwardSource::FROM_EXECUTE;
                std::cout << "[HDU] FORWARD-A: EX->EX (Reg " << (int)rs1 << ")" << std::endl;
            }
            // Priority 2: MEM Stage
            else if (ex_mem.valid && ex_mem.signals.reg_write_ && ex_mem.des_address != 0 && ex_mem.des_address == rs1) {
                signals.forward_A = ForwardSource::FROM_MEMORY;
                std::cout << "[HDU] FORWARD-A: MEM->EX (Reg " << (int)rs1 << ")" << std::endl;
            }
            else {
                // DEBUGGING BLOCK FOR CYCLE 16 FAILURE
                // If we needed rs1, and EX failed, and MEM failed, let's check why MEM failed if it looks valid.
                /*
                if (ex_mem.valid && ex_mem.des_address == rs1) {
                   std::cout << "[HDU DEBUG] RS1 Match in MEM but failed conditions!" << std::endl;
                   std::cout << "  RegWrite: " << ex_mem.signals.reg_write_ << std::endl;
                   std::cout << "  DesAddr: " << (int)ex_mem.des_address << " RS1: " << (int)rs1 << std::endl;
                }
                */
            }
        }

        // --- Forward B (RS2) ---
        if (use_rs2) {
            if (id_ex_reg.valid && id_ex_reg.signals.reg_write_ && id_ex_reg.rd != 0 && id_ex_reg.rd == rs2) {
                signals.forward_B = ForwardSource::FROM_EXECUTE;
                std::cout << "[HDU] FORWARD-B: EX->EX (Reg " << (int)rs2 << ")" << std::endl;
            }
            else if (ex_mem.valid && ex_mem.signals.reg_write_ && ex_mem.des_address != 0 && ex_mem.des_address == rs2) {
                signals.forward_B = ForwardSource::FROM_MEMORY;
                std::cout << "[HDU] FORWARD-B: MEM->EX (Reg " << (int)rs2 << ")" << std::endl;
            }
        }
    }

    return signals;
}