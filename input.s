# Test Code

# test_full_hazards.s
#
# This program is designed to test all data hazards WITHOUT
# any control hazards (no branches).
# It calculates a simple polynomial: y = ax^2 + bx + c
#
# It should produce:
# Mode 1 (No Hazard): WRONG result (will be 0)
# Mode 2 (Stall-Only): CORRECT result (x9 = 390), but VERY SLOW.
# Mode 3 (Forwarding): CORRECT result (x9 = 390), but MUCH FASTER.

.data
poly_storage: .dword 0
    
.text
    # --- 1. Setup values ---
    addi x1, x0, 10     # x1 = a (10)
    addi x2, x0, 5      # x2 = x (5)
    addi x3, x0, 20     # x3 = b (20)
    addi x4, x0, 40     # x4 = c (40)
    
    # --- 2. Create a long chain of ALU-to-ALU data hazards ---
    # These hazards can be fixed by forwarding.
    
    mul x5, x1, x2      # x5 = a*x (10*5 = 50)
                        # HAZARD: Needs x1, x2 from 2-3 cycles ago
                        
    mul x6, x5, x2      # x6 = ax*x (50*5 = 250) (this is ax^2)
                        # HAZARD: Needs x5, x2 from 1-2 cycles ago
                        
    mul x7, x3, x2      # x7 = b*x (20*5 = 100)
                        # HAZARD: Needs x3, x2
                        
    add x8, x6, x7      # x8 = ax^2 + bx (250 + 100 = 350)
                        # HAZARD: Needs x6, x7
                        
    add x9, x8, x4      # x9 = ax^2 + bx + c (350 + 40 = 390)
                        # HAZARD: Needs x8, x4
                        
    # --- 3. Create a Load-Use hazard ---
    # This hazard MUST cause a stall, even with forwarding.
    
    la x10, poly_storage
    sd x2, 0(x10)       # Store x (5) to memory
    ld x11, 0(x10)      # Load x (5) from memory into x11


    
    addi x12, x11, 1    # x12 = x11 + 1 (5 + 1 = 6)
                        # HAZARD: Needs x11 from the ld instruction

