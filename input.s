.text
main:
    addi x1, x0, 10
    addi x2, x0, 20
    
    # Forwarding Test
    add  x3, x1, x2   # x3 = 30
    add  x4, x3, x1   # x4 = 40 (Uses x3 immediately)
    
    # Load-Use Test
    sw   x4, 0(x0)
    lw   x5, 0(x0)
    add  x6, x5, x1   # x6 = 50 (Stall required)
