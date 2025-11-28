# 5-Stage Pipelined RISC-V Simulator

This project implements a 5-Stage Pipelined RISC-V Processor Simulator in C++, supporting integer and floating-point instructions with hazard detection, forwarding, and branching. The simulator can be run in both CLI and GUI modes to visualize pipeline behavior and register states.

---

## How to Build & Run

### Build
    mkdir build
    cd build
    cmake ..
    make

### CLI Mode
Run the simulator from the build directory:

    ./vm --start-vm

Within the interactive shell, you can:

    load ../examples/stress_test.s
    modify_config pipelining mode 3
    run

### GUI Mode
Run the Python GUI:

    python3 python_script.py

Steps:
1. Load an assembly or machine code file.
2. Step through the simulation cycle-by-cycle or run to completion.
3. Observe the pipeline stages and register file updates live.
