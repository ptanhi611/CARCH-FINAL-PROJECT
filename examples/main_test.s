# --- 1. Integer Forwarding Test (EX->EX) ---
    addi x1, x0, 10       # x1 = 10
    addi x2, x0, 20       # x2 = 20
    add  x3, x1, x2       # x3 = 10 + 20 = 30. (Depends on previous writes)
    
    # --- 2. Load-Use Stall Test (MEM->EX) ---
    sw   x3, 0(x0)        # Store 30 to memory address 0
    lw   x4, 0(x0)        # Load 30 into x4
    addi x5, x4, 10       # x5 = 30 + 10 = 40. 
                          # CRITICAL: This MUST STALL because x4 is loaded just before.

    # --- 3. Loop/Branch Hazard Test ---
    addi x6, x0, 3        # Loop counter = 3
loop:
    addi x6, x6, -1       # Decrement. x6 is modified in EX stage
    bne  x6, x0, loop     # Branch depends on x6. 
                          # CRITICAL: MUST STALL in Decode to wait for x6.

    # --- 4. Float Setup & Address Calc Test ---
    # We manually create float 1.0 (0x3F800000) using integer instructions
    lui  x7, 260096       # 260096 is 0x3F800 (Upper 20 bits)
    sw   x7, 100(x0)      # Write to address 100

    # FLW: Test if Integer ALU calculates Address 100 correctly for Float
    flw  f1, 100(x0)      # f1 = 1.0

    # --- 5. Float Forwarding Test ---
    fadd.s f2, f1, f1     # f2 = 1.0 + 1.0 = 2.0
    fadd.s f3, f2, f2     # f3 = 2.0 + 2.0 = 4.0. 
                          # CRITICAL: f2 is needed immediately. Forwarding check!

    # --- 6. Float Store Test ---
    # FSW: Test if Integer ALU calculates Address 104 correctly for Store
    fsw  f3, 104(x0)      # Store 4.0 to address 104

    # End (Infinite Loop)
done:
    beq  x0, x0, done