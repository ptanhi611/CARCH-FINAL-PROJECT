import sys
import json
import os
import re

from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, 
                             QHBoxLayout, QLabel, QTableWidget, QTableWidgetItem, 
                             QGroupBox, QFrame, QPlainTextEdit, QPushButton,
                             QComboBox, QSplitter, QTabWidget, QAbstractItemView, QGridLayout,
                             QTextEdit)  # <-- QTextEdit belongs in QtWidgets
from PyQt6.QtCore import QTimer, Qt, QProcess, QRect, QSize
from PyQt6.QtGui import QFont, QColor, QSyntaxHighlighter, QTextCharFormat, QPainter, QTextCursor, QTextFormat, QBrush


# --- CONFIGURATION ---

SIM_EXECUTABLE = "./build/vm" 
BASE_DIR = os.path.abspath(os.getcwd())
INPUT_ASM_FILE = os.path.join(BASE_DIR, "input.s")
CONFIG_FILE = os.path.join(BASE_DIR, "config.json")


# Robust Paths: Check root AND vm_state folder

STATE_FILENAMES = ["vm_state_dump.json", "vm_state/vm_state_dump.json"]
REGISTER_FILENAMES = ["registers.json", "vm_state/registers.json"]


# --- THEME COLORS ---

BG_COLOR = "#f8f9fa"
PANEL_BG = "#ffffff"
TEXT_COLOR = "#2c3e50"
ACCENT_COLOR = "#2980b9" 
BORDER_COLOR = "#bdc3c7"

# ensure EDITOR_BG exists (matches original styling intent)
EDITOR_BG = PANEL_BG


# Pipeline Highlight Colors (Vibrant for visibility)

COLOR_IF  = QColor(52, 152, 219, 80)   # Blue
COLOR_ID  = QColor(243, 156, 18, 80)   # Orange
COLOR_EX  = QColor(231, 76, 60, 80)    # Red
COLOR_MEM = QColor(155, 89, 182, 80)   # Purple
COLOR_WB  = QColor(46, 204, 113, 80)   # Green


STYLESHEET = f"""

QMainWindow {{ background-color: {BG_COLOR}; }}

QWidget {{ color: {TEXT_COLOR}; font-family: 'Segoe UI', sans-serif; font-size: 13px; }}

QGroupBox {{ 

    border: 1px solid {BORDER_COLOR}; 

    border-radius: 6px; 

    margin-top: 24px; 

    background-color: {PANEL_BG}; 

    font-weight: bold;

}}

QGroupBox::title {{ 

    subcontrol-origin: margin; 

    left: 10px; 

    padding: 0 5px; 

    color: {ACCENT_COLOR}; 

}}

QPushButton {{ 

    background-color: #ffffff; 

    border: 1px solid {BORDER_COLOR}; 

    padding: 6px 12px; 

    border-radius: 4px; 

    color: #333; 

    font-weight: bold;

}}

QPushButton:hover {{ background-color: #eef2f5; border-color: {ACCENT_COLOR}; }}

QPushButton:pressed {{ background-color: {ACCENT_COLOR}; color: white; }}

QPushButton:disabled {{ background-color: #f0f0f0; color: #aaa; border-color: #ddd; }}

QTableWidget {{ 

    background-color: {PANEL_BG}; 

    gridline-color: {BORDER_COLOR}; 

    border: 1px solid {BORDER_COLOR}; 

    font-family: Consolas; 

}}

QHeaderView::section {{ 

    background-color: #ecf0f1; 

    padding: 4px; 

    border: 1px solid {BORDER_COLOR}; 

    color: #333; 

    font-weight: bold; 

}}

QComboBox {{
    background-color: #ffffff;
    color: #000000;
    padding: 4px;
    border: 1px solid #bdc3c7;
}}

QComboBox QAbstractItemView {{
    background-color: #ffffff;
    color: #000000;
}}


QPlainTextEdit {{ 

    background-color: {PANEL_BG}; 

    color: {TEXT_COLOR}; 

    border: 1px solid {BORDER_COLOR}; 

    font-family: Consolas; 

    font-size: 14px; 

}}

QLabel#StatVal {{ font-family: Consolas; font-size: 16px; color: {ACCENT_COLOR}; font-weight: bold; }}

"""


# --- EDITOR HELPERS ---

class LineNumberArea(QWidget):

    def __init__(self, editor):

        super().__init__(editor)

        self.codeEditor = editor


    def sizeHint(self):

        return QSize(self.codeEditor.lineNumberAreaWidth(), 0)


    def paintEvent(self, event):

        self.codeEditor.lineNumberAreaPaintEvent(event)


class CodeEditor(QPlainTextEdit):

    def __init__(self):

        super().__init__()

        self.lineNumberArea = LineNumberArea(self)

        self.blockCountChanged.connect(self.updateLineNumberAreaWidth)

        self.updateRequest.connect(self.updateLineNumberArea)

        self.updateLineNumberAreaWidth(0)

        self.pipeline_stages = {} # Line -> Stage Name


    def lineNumberAreaWidth(self):

        digits = 1; max_num = max(1, self.blockCount())

        while max_num >= 10: max_num //= 10; digits += 1

        return 40 + self.fontMetrics().horizontalAdvance('9') * digits # Wider for stage labels


    def updateLineNumberAreaWidth(self, _):

        self.setViewportMargins(self.lineNumberAreaWidth(), 0, 0, 0)


    def updateLineNumberArea(self, rect, dy):

        if dy: self.lineNumberArea.scroll(0, dy)

        else: self.lineNumberArea.update(0, rect.y(), self.lineNumberArea.width(), rect.height())

        if rect.contains(self.viewport().rect()): self.updateLineNumberAreaWidth(0)


    def resizeEvent(self, event):

        super().resizeEvent(event)

        cr = self.contentsRect()

        self.lineNumberArea.setGeometry(QRect(cr.left(), cr.top(), self.lineNumberAreaWidth(), cr.height()))


    def lineNumberAreaPaintEvent(self, event):

        painter = QPainter(self.lineNumberArea)

        painter.fillRect(event.rect(), QColor("#ecf0f1"))

        

        block = self.firstVisibleBlock()

        blockNumber = block.blockNumber()

        top = round(self.blockBoundingGeometry(block).translated(self.contentOffset()).top())

        bottom = top + round(self.blockBoundingRect(block).height())

        

        while block.isValid() and top <= event.rect().bottom():

            if block.isVisible() and bottom >= event.rect().top():

                # Draw Line Number

                painter.setPen(QColor("#7f8c8d"))

                painter.setFont(QFont("Consolas", 10))

                painter.drawText(0, top, self.lineNumberArea.width() - 35, self.fontMetrics().height(),

                                 Qt.AlignmentFlag.AlignRight, str(blockNumber + 1))

                

                # Draw Stage Label if present (e.g. "IF", "EX")

                if blockNumber in self.pipeline_stages:

                    stage_name, color = self.pipeline_stages[blockNumber]

                    

                    # Background Bubble

                    painter.setBrush(QBrush(color))

                    painter.setPen(Qt.PenStyle.NoPen)

                    rect_x = self.lineNumberArea.width() - 30

                    painter.drawRoundedRect(rect_x, top + 2, 25, 14, 3, 3)

                    

                    # Text

                    painter.setPen(QColor("white"))

                    painter.setFont(QFont("Segoe UI", 8, QFont.Weight.Bold))

                    painter.drawText(rect_x, top + 2, 25, 14, Qt.AlignmentFlag.AlignCenter, stage_name)


            block = block.next()

            top = bottom

            bottom = top + round(self.blockBoundingRect(block).height())

            blockNumber += 1


class AssemblyHighlighter(QSyntaxHighlighter):

    def __init__(self, doc):

        super().__init__(doc)

        self.rules = []

        fmt = QTextCharFormat(); fmt.setForeground(QColor("#0000ff")); fmt.setFontWeight(QFont.Weight.Bold)

        for w in ["add","addi","sub","lw","sw","beq","bne","jal","jalr","la","li","ecall","flw","fsw","fadd.s","fadd.d","fmul.s","fdiv.s"]:

            self.rules.append((f"\\b{w}\\b", fmt))

        fmt = QTextCharFormat(); fmt.setForeground(QColor("#a31515"))

        self.rules.append(("x[0-9]+|f[0-9]+|a[0-7]|t[0-6]|sp|ra|zero", fmt))

        fmt = QTextCharFormat(); fmt.setForeground(QColor("#008000")); fmt.setFontItalic(True)

        self.rules.append(("#.*", fmt))

        fmt = QTextCharFormat(); fmt.setForeground(QColor("#800080"))

        self.rules.append(("\\.[a-zA-Z]+", fmt))


    def highlightBlock(self, text):

        import re

        for pattern, fmt in self.rules:

            for match in re.finditer(pattern, text):

                self.setFormat(match.start(), match.end()-match.start(), fmt)


# --- MAIN WINDOW ---

class IDEWindow(QMainWindow):

    def __init__(self):

        super().__init__()

        self.setWindowTitle("RISC-V Pipeline Simulator")

        self.resize(1400, 900)

        self.setStyleSheet(STYLESHEET)

        self.prev_regs = {}

        self.sim_process = None

        self.pc_to_line_map = {}

        self.base_pc_detected = False # Flag to auto-detect start address

        

        self.init_ui()


    def init_ui(self):

        main_widget = QWidget()

        self.setCentralWidget(main_widget)

        layout = QHBoxLayout(main_widget)


        # --- LEFT PANEL: Editor ---

        left_layout = QVBoxLayout()

        

        # Toolbar

        toolbar = QHBoxLayout()

        self.btn_load = QPushButton("🛠 Load / Reset"); self.btn_load.clicked.connect(self.do_load)

        self.combo_mode = QComboBox()

        self.combo_mode.addItems(["Mode 0: Single", "Mode 1: No Haz", "Mode 2: Stall", "Mode 3: Fwd", "Mode 4: B-Static", "Mode 5: B-Dynamic"])

        self.combo_mode.setCurrentIndex(3)

        toolbar.addWidget(self.btn_load)

        toolbar.addWidget(self.combo_mode)

        toolbar.addStretch()

        left_layout.addLayout(toolbar)


        # Editor

        editor_grp = QGroupBox("Assembly Source")

        el = QVBoxLayout(editor_grp)

        self.editor = CodeEditor()

        self.highlighter = AssemblyHighlighter(self.editor.document())

        self.editor.setPlainText("""# Test Code

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

""")

        el.addWidget(self.editor)

        left_layout.addWidget(editor_grp)

        

        # Logs

        log_grp = QGroupBox("Simulator Logs")

        ll = QVBoxLayout(log_grp)

        self.console = QPlainTextEdit()

        self.console.setReadOnly(True)

        ll.addWidget(self.console)

        left_layout.addWidget(log_grp)

        left_layout.setStretch(1, 3)

        left_layout.setStretch(2, 1)


        # --- RIGHT PANEL: Controls & Data ---

        right_layout = QVBoxLayout()

        

        # Controls

        ctrl_grp = QGroupBox("Simulation Control")

        cl = QVBoxLayout(ctrl_grp)

        

        # Playback

        pb_layout = QHBoxLayout()

        self.btn_step = QPushButton("⤵ Step"); self.btn_step.clicked.connect(self.do_step)

        self.btn_run = QPushButton("▶ Run"); self.btn_run.clicked.connect(self.do_run)

        self.btn_stop = QPushButton("⏸ Pause"); self.btn_stop.clicked.connect(self.do_pause)

        pb_layout.addWidget(self.btn_step); pb_layout.addWidget(self.btn_run); pb_layout.addWidget(self.btn_stop)

        cl.addLayout(pb_layout)

        

        # Time Travel

        tt_layout = QHBoxLayout()

        self.btn_undo = QPushButton("⟲ Undo"); self.btn_undo.clicked.connect(self.do_undo)

        self.btn_redo = QPushButton("⟳ Redo"); self.btn_redo.clicked.connect(self.do_redo)

        tt_layout.addWidget(self.btn_undo); tt_layout.addWidget(self.btn_redo)

        cl.addLayout(tt_layout)

        

        # Speed

        sp_layout = QHBoxLayout()

        sp_layout.addWidget(QLabel("Speed:"))

        self.combo_speed = QComboBox()

        self.combo_speed.addItems(["Slow (1s)", "Normal (200ms)", "Fast (100ms)", "Max (10ms)"])

        self.combo_speed.setCurrentIndex(1)

        sp_layout.addWidget(self.combo_speed)

        cl.addLayout(sp_layout)

        

        right_layout.addWidget(ctrl_grp)


        # Statistics

        stat_grp = QGroupBox("Statistics")

        sl = QGridLayout(stat_grp)

        self.v_cycle = QLabel("0"); self.v_cycle.setObjectName("StatVal")

        self.v_stall = QLabel("0"); self.v_stall.setObjectName("StatVal")

        self.v_bpu   = QLabel("-"); self.v_bpu.setObjectName("StatVal")

        

        sl.addWidget(QLabel("Cycle:"), 0, 0); sl.addWidget(self.v_cycle, 0, 1)

        sl.addWidget(QLabel("Stalls:"), 1, 0); sl.addWidget(self.v_stall, 1, 1)

        sl.addWidget(QLabel("BPU:"), 2, 0); sl.addWidget(self.v_bpu, 2, 1)

        right_layout.addWidget(stat_grp)


        # Registers

        reg_grp = QGroupBox("Registers")

        rl = QVBoxLayout(reg_grp)

        self.reg_table = QTableWidget(32, 3)

        self.reg_table.setHorizontalHeaderLabels(["Reg", "Value (Hex)", "Value (Dec)"])

        self.reg_table.verticalHeader().setVisible(False)

        self.reg_table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)

        

        for i in range(32):

            self.reg_table.setItem(i, 0, QTableWidgetItem(f"x{i}"))

            self.reg_table.setItem(i, 1, QTableWidgetItem("0"))

            self.reg_table.setItem(i, 2, QTableWidgetItem("0"))

            

        rl.addWidget(self.reg_table)

        right_layout.addWidget(reg_grp)

        right_layout.setStretch(3, 1) # Registers take available space


        # Combine

        layout.addLayout(left_layout, 60)

        layout.addLayout(right_layout, 40)


        # Timers

        self.timer = QTimer()

        self.timer.timeout.connect(self.update_gui)

        self.run_timer = QTimer()

        self.run_timer.timeout.connect(self.do_step)


    # --- LOGIC ---


    def map_pc_to_lines(self, start_pc=0):

        """Maps PC addresses to line numbers assuming 4-byte instructions"""

        text = self.editor.toPlainText()

        lines = text.split('\n')

        self.pc_to_line_map = {}

        

        current_pc = start_pc

        

        # Heuristic: Scan line by line. If it looks like an instruction, map it.

        for i, line in enumerate(lines):

            clean = line.strip()

            if not clean: continue

            if clean.startswith("#") or clean.startswith("."): continue

            if clean.endswith(":"): continue # Label

            

            # It's likely an instruction

            self.pc_to_line_map[current_pc] = i

            current_pc += 4


    def highlight_pipeline_stages(self, pipe_data):

        self.editor.pipeline_stages = {} # Clear side labels

        selections = []

        

        stages = [

            ("fetch", COLOR_IF, "IF"), 

            ("decode", COLOR_ID, "ID"), 

            ("execute", COLOR_EX, "EX"), 

            ("memory", COLOR_MEM, "MEM"), 

            ("writeback", COLOR_WB, "WB")

        ]

        

        # First pass: detect if we need to offset PC

        # If the simulator says PC is 0x10000, but we mapped from 0x0, we fix it.

        if not self.base_pc_detected:

            # Try to find a valid PC from any stage to set baseline

            for s, _, _ in stages:

                pc = pipe_data.get(s, {}).get("pc")

                if pc is not None and pc > 0:

                    self.map_pc_to_lines(start_pc=pc) # Re-map with correct start

                    self.base_pc_detected = True

                    break


        for stage_name, color, label in stages:

            stage_info = pipe_data.get(stage_name, {})

            pc = stage_info.get("pc")

            desc = stage_info.get("desc", "")

            

            if pc is not None and "BUBBLE" not in desc:

                line_num = self.pc_to_line_map.get(pc)

                if line_num is not None:

                    # 1. Background Highlight

                    sel = QTextEdit.ExtraSelection()

                    sel.format.setBackground(color)

                    # sel.format.setProperty(QTextFormat.Property.FullWidthSelection, True)

                    doc = self.editor.document()

                    block = doc.findBlockByNumber(line_num)

                    sel.cursor = QTextCursor(block)

                    selections.append(sel)

                    

                    # 2. Side Label (stored in editor class for paintEvent)

                    self.editor.pipeline_stages[line_num] = (label, color)

        

        self.editor.setExtraSelections(selections)

        self.editor.lineNumberArea.update() # Force redraw of labels


    def do_load(self):

        try:

            with open(INPUT_ASM_FILE, "w") as f: f.write(self.editor.toPlainText())

        except Exception as e: self.console.appendPlainText(f"Save Error: {e}"); return


        config = {"pipeline_mode": self.combo_mode.currentIndex(), "input_program": INPUT_ASM_FILE}

        with open(CONFIG_FILE, "w") as f: json.dump(config, f)


        # Reset State

        self.base_pc_detected = False

        self.map_pc_to_lines(0) # Default start 0

        self.editor.setExtraSelections([])

        self.editor.pipeline_stages = {}

        self.v_bpu.setText("-")


        if self.sim_process: self.sim_process.kill()

        self.sim_process = QProcess()

        self.sim_process.setProgram(SIM_EXECUTABLE)

        self.sim_process.setArguments([CONFIG_FILE, "--start-vm"])

        self.sim_process.readyReadStandardOutput.connect(self.handle_stdout)

        self.sim_process.readyReadStandardError.connect(self.handle_stderr)

        self.sim_process.start()

        

        # <<< FIX: lbl_status was missing in your original; added here only >>>
        self.lbl_status = getattr(self, "lbl_status", None)
        if not self.lbl_status:
            # If label wasn't created earlier (safe fallback), create it and attach to UI minimally:
            # But prefer to set if already exists.
            # We'll setText if it exists; otherwise simply update a variable so future code doesn't crash.
            self.lbl_status = QLabel("Simulator Loaded")
        else:
            self.lbl_status.setText("Simulator Loaded")
        # <<< end fix >>>

        QTimer.singleShot(100, lambda: self.send_cmd(f"load {INPUT_ASM_FILE}"))

        self.timer.start(50) 


    def send_cmd(self, cmd):

        if self.sim_process and self.sim_process.state() == QProcess.ProcessState.Running:

            self.sim_process.write(f"{cmd}\n".encode())


    def do_step(self): self.send_cmd("step")

    def do_undo(self): self.do_pause(); self.send_cmd("undo")

    def do_redo(self): self.do_pause(); self.send_cmd("redo")

    

    def do_run(self):

        delays = [1000, 500, 100, 10]

        self.run_timer.start(delays[self.combo_speed.currentIndex()])

        # update status label only if it exists in UI hierarchy
        try:
            self.lbl_status.setText("Running...")
        except Exception:
            pass

        self.btn_run.setEnabled(False); self.btn_stop.setEnabled(True)


    def do_pause(self):

        self.run_timer.stop()

        try:
            self.lbl_status.setText("Paused")
        except Exception:
            pass

        self.btn_run.setEnabled(True); self.btn_stop.setEnabled(False)


    def handle_stdout(self):

        data = self.sim_process.readAllStandardOutput().data().decode()

        if data.strip(): self.console.appendPlainText(data.strip())


    def handle_stderr(self):

        data = self.sim_process.readAllStandardError().data().decode()

        self.console.appendPlainText(f"ERR: {data.strip()}")


    def update_gui(self):

        # 1. Pipeline Data

        state_file = None

        for f in STATE_FILENAMES:

            if os.path.exists(f): state_file = f; break

            

        if state_file:

            try:

                with open(state_file, 'r') as f: data = json.load(f)

                self.v_cycle.setText(str(data.get('cycle', 0)))

                self.v_stall.setText(str(data.get('stall_count', 0)))

                

                bpu_acc = data.get("bpu_accuracy", None)

                if bpu_acc is not None: self.v_bpu.setText(f"{bpu_acc:.1f}%")

                

                self.highlight_pipeline_stages(data.get("pipeline", {}))

            except: pass


        # 2. Registers Data (Robust Check)

        reg_file = None

        for f in REGISTER_FILENAMES:

            if os.path.exists(f): reg_file = f; break

            

        if reg_file:

            try:

                with open(reg_file, 'r') as f: regs = json.load(f)

                

                # Handling multiple JSON formats (Array vs Dict)

                if "GPR" in regs:

                    gpr_data = regs["GPR"]

                    if isinstance(gpr_data, list):

                        for i, val in enumerate(gpr_data):

                            if i < 32: self.update_reg_item(i, 1, f"x{i}", val)

                    elif isinstance(gpr_data, dict):

                        for k, v in gpr_data.items():

                            idx = int(k.replace('x', ''))

                            if idx < 32: self.update_reg_item(idx, 1, k, v)

                            

                # Fallback if root keys are x0, x1...

                elif "x0" in regs:

                    for i in range(32):

                        key = f"x{i}"

                        if key in regs: self.update_reg_item(i, 1, key, regs[key])

                        

            except Exception as e:

                # self.console.appendPlainText(f"Reg Read Error: {e}")

                pass


    def update_reg_item(self, row, col, name, val):

        item = self.reg_table.item(row, col)

        if not item: return

        

        hex_val = f"0x{int(val):x}" if isinstance(val, (int, float)) else str(val)

        

        # Check change

        old = self.prev_regs.get(name)

        if old is not None and old != val:

            item.setBackground(QColor("#27ae60")); item.setForeground(QColor("white"))

        else:

            item.setBackground(QColor(EDITOR_BG)); item.setForeground(QColor(TEXT_COLOR))

        

        item.setText(hex_val)

        

        # Update Decimal Column

        if col == 1:

            dec_item = self.reg_table.item(row, 2)

            if dec_item: 

                dec_item.setText(str(val))

             

        self.prev_regs[name] = val


if __name__ == "__main__":

    app = QApplication(sys.argv)

    app.setStyle("Fusion")

    if hasattr(Qt.ApplicationAttribute, 'AA_EnableHighDpiScaling'):

        QApplication.setAttribute(Qt.ApplicationAttribute.AA_EnableHighDpiScaling, True)

    win = IDEWindow()
    win.show()
    sys.exit(app.exec())
