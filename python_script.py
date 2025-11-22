import sys
import json
import os
import subprocess
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, 
                             QHBoxLayout, QLabel, QTableWidget, QTableWidgetItem, 
                             QGroupBox, QHeaderView, QFrame, QPlainTextEdit, QPushButton,
                             QComboBox, QSplitter, QTabWidget, QStyle, QSizePolicy)
from PyQt6.QtCore import QTimer, Qt, QProcess, QRect, QSize
from PyQt6.QtGui import QFont, QColor, QSyntaxHighlighter, QTextCharFormat, QPainter, QTextFormat, QPalette, QPen, QBrush

# --- CONFIGURATION ---
SIM_EXECUTABLE = "./build/vm" 
INPUT_ASM_FILE = "input.s"
CONFIG_FILE = "config.json"
# We will search for these files automatically in the update loop
STATE_FILENAME = "vm_state_dump.json"
REGISTER_FILENAME = "vm_state / registers_dump.json"

# --- STYLING (Dark Theme) ---
DARK_STYLE = """
QMainWindow { background-color: #2b2b2b; }
QWidget { color: #ecf0f1; font-family: 'Segoe UI', sans-serif; }
QGroupBox { border: 1px solid #444; border-radius: 5px; margin-top: 20px; font-weight: bold; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 3px; color: #3498db; }
QPushButton { background-color: #34495e; border: none; padding: 8px; border-radius: 4px; color: white; }
QPushButton:hover { background-color: #4e6a85; }
QPushButton:disabled { background-color: #2c3e50; color: #7f8c8d; }
QTableWidget { background-color: #1e1e1e; gridline-color: #333; border: none; }
QHeaderView::section { background-color: #333; padding: 4px; border: none; color: #ddd; }
QTabWidget::pane { border: 1px solid #444; }
QTabBar::tab { background: #333; color: #aaa; padding: 8px 12px; border-top-left-radius: 4px; border-top-right-radius: 4px; }
QTabBar::tab:selected { background: #2b2b2b; color: white; font-weight: bold; }
QComboBox { background-color: #1e1e1e; border: 1px solid #444; padding: 5px; }
QScrollBar { background: #2b2b2b; }
"""

# --- EDITOR COMPONENTS ---
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
        
        self.setFont(QFont("Consolas", 12))
        self.setStyleSheet("background-color: #1e1e1e; color: #d4d4d4; border: none;")

    def lineNumberAreaWidth(self):
        digits = 1
        max_num = max(1, self.blockCount())
        while max_num >= 10:
            max_num //= 10
            digits += 1
        space = 3 + self.fontMetrics().horizontalAdvance('9') * digits
        return space

    def updateLineNumberAreaWidth(self, _):
        self.setViewportMargins(self.lineNumberAreaWidth(), 0, 0, 0)

    def updateLineNumberArea(self, rect, dy):
        if dy:
            self.lineNumberArea.scroll(0, dy)
        else:
            self.lineNumberArea.update(0, rect.y(), self.lineNumberArea.width(), rect.height())
        
        # Fix for PyQt recursion crash
        if rect.contains(self.viewport().rect()):
            self.updateLineNumberAreaWidth(0)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        cr = self.contentsRect()
        self.lineNumberArea.setGeometry(QRect(cr.left(), cr.top(), self.lineNumberAreaWidth(), cr.height()))

    def lineNumberAreaPaintEvent(self, event):
        painter = QPainter(self.lineNumberArea)
        painter.fillRect(event.rect(), QColor("#252526"))
        block = self.firstVisibleBlock()
        blockNumber = block.blockNumber()
        top = round(self.blockBoundingGeometry(block).translated(self.contentOffset()).top())
        bottom = top + round(self.blockBoundingRect(block).height())

        while block.isValid() and top <= event.rect().bottom():
            if block.isVisible() and bottom >= event.rect().top():
                number = str(blockNumber + 1)
                painter.setPen(QColor("#858585"))
                painter.drawText(0, top, self.lineNumberArea.width() - 2, self.fontMetrics().height(),
                                 Qt.AlignmentFlag.AlignRight, number)
            block = block.next()
            top = bottom
            bottom = top + round(self.blockBoundingRect(block).height())
            blockNumber += 1

class AssemblyHighlighter(QSyntaxHighlighter):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.rules = []
        
        c_keyword = QTextCharFormat(); c_keyword.setForeground(QColor("#569cd6")); c_keyword.setFontWeight(QFont.Weight.Bold)
        c_reg = QTextCharFormat(); c_reg.setForeground(QColor("#4ec9b0"))
        c_comment = QTextCharFormat(); c_comment.setForeground(QColor("#6a9955"))
        c_number = QTextCharFormat(); c_number.setForeground(QColor("#b5cea8"))
        c_label = QTextCharFormat(); c_label.setForeground(QColor("#dcdcaa"))

        keywords = ["add", "sub", "addi", "lw", "sw", "beq", "bne", "jal", "jalr", "flw", "fsw", "la", "li", "ecall", "fadd.s", "fadd.d", "fmul.s", "fdiv.s", "fsd", "fld"]
        self.rules.append((f"\\b({'|'.join(keywords)})\\b", c_keyword))
        self.rules.append(("x[0-9]+|f[0-9]+|a[0-7]|t[0-6]|s[0-11]|zero|ra|sp|gp|tp", c_reg))
        self.rules.append((r"#[^\n]*", c_comment))
        self.rules.append((r"\b[0-9]+\b|0x[0-9a-fA-F]+", c_number))
        self.rules.append((r"^[a-zA-Z0-9_]+:", c_label))

    def highlightBlock(self, text):
        import re
        for pattern, fmt in self.rules:
            for match in re.finditer(pattern, text):
                self.setFormat(match.start(), match.end() - match.start(), fmt)

# --- VISUALIZER COMPONENTS ---
class PipelineGraph(QWidget):
    def __init__(self):
        super().__init__()
        self.stages = {
            "fetch": {"name": "IF", "color": "#3498db", "data": {}},
            "decode": {"name": "ID", "color": "#e67e22", "data": {}},
            "execute": {"name": "EX", "color": "#e74c3c", "data": {}},
            "memory": {"name": "MEM", "color": "#9b59b6", "data": {}},
            "writeback": {"name": "WB", "color": "#2ecc71", "data": {}}
        }
        self.setMinimumHeight(200)

    def update_data(self, pipeline_data):
        for key in self.stages:
            self.stages[key]["data"] = pipeline_data.get(key, {})
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        margin = 20
        box_width = (self.width() - (margin * 6)) / 5
        box_height = 120
        y_pos = 40
        
        keys = list(self.stages.keys())
        for i, key in enumerate(keys):
            stage = self.stages[key]
            x_pos = margin + (i * (box_width + margin))
            
            if i < len(keys) - 1:
                painter.setPen(QPen(QColor("#555"), 2))
                mid_y = y_pos + box_height / 2
                painter.drawLine(int(x_pos + box_width), int(mid_y), int(x_pos + box_width + margin), int(mid_y))
                painter.drawLine(int(x_pos + box_width + margin - 5), int(mid_y - 5), int(x_pos + box_width + margin), int(mid_y))
                painter.drawLine(int(x_pos + box_width + margin - 5), int(mid_y + 5), int(x_pos + box_width + margin), int(mid_y))

            data = stage["data"]
            desc = data.get("desc", "NOP")
            is_bubble = "BUBBLE" in desc or ("0x0" in desc and len(desc) < 5)
            
            bg_color = QColor(stage["color"])
            if is_bubble:
                bg_color = QColor("#444") 
                border_color = QColor("#e74c3c") 
            else:
                bg_color = bg_color.darker(150)
                border_color = QColor(stage["color"])

            rect = QRect(int(x_pos), int(y_pos), int(box_width), int(box_height))
            
            painter.setBrush(QBrush(bg_color))
            painter.setPen(QPen(border_color, 2))
            painter.drawRoundedRect(rect, 8, 8)
            
            header_rect = QRect(int(x_pos), int(y_pos), int(box_width), 30)
            painter.setBrush(QBrush(border_color))
            painter.drawRoundedRect(header_rect, 8, 8)
            
            painter.setPen(QColor("white"))
            painter.setFont(QFont("Segoe UI", 10, QFont.Weight.Bold))
            painter.drawText(header_rect, Qt.AlignmentFlag.AlignCenter, stage["name"])
            
            content_rect = QRect(int(x_pos + 5), int(y_pos + 35), int(box_width - 10), int(box_height - 40))
            painter.setFont(QFont("Consolas", 9))
            
            text = desc if not is_bubble else "BUBBLE"
            if "pc" in data: text += f"\nPC: {hex(data['pc'])}"
            if "alu_result" in data and not is_bubble: text += f"\nRes: {hex(data['alu_result'])}"
            if "wb_value" in data and not is_bubble: text += f"\nVal: {hex(data['wb_value'])}"
            
            if data.get("forward_a", 0) > 0: text += "\nFWD A: ON"
            if data.get("forward_b", 0) > 0: text += "\nFWD B: ON"

            painter.drawText(content_rect, Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter, text)


# --- MAIN WINDOW ---
class IDEWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("RISC-V Pipeline Studio Pro")
        self.resize(1400, 850)
        self.setStyleSheet(DARK_STYLE)
        
        self.prev_regs = {} 

        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        layout = QVBoxLayout(main_widget)

        # --- TOOLBAR ---
        toolbar = QHBoxLayout()
        
        title = QLabel("RVSS")
        title.setStyleSheet("font-size: 20px; font-weight: bold; color: #3498db; margin-right: 20px;")
        toolbar.addWidget(title)

        self.combo_mode = QComboBox()
        self.combo_mode.addItems(["Mode 0: Single Cycle", "Mode 1: No Hazard", "Mode 2: Stall Only", "Mode 3: Forwarding", "Mode 4: Branch (Static)", "Mode 5: Branch (Dynamic)"])
        self.combo_mode.setCurrentIndex(3)
        self.combo_mode.setMinimumWidth(200)
        toolbar.addWidget(self.combo_mode)
        
        self.btn_run = QPushButton("▶ Build & Run")
        self.btn_run.setStyleSheet("background-color: #27ae60;")
        self.btn_run.clicked.connect(self.run_sim)
        toolbar.addWidget(self.btn_run)
        
        self.btn_stop = QPushButton("⏹ Stop")
        self.btn_stop.setStyleSheet("background-color: #c0392b;")
        self.btn_stop.clicked.connect(self.stop_sim)
        self.btn_stop.setEnabled(False)
        toolbar.addWidget(self.btn_stop)
        
        toolbar.addStretch()
        
        self.lbl_cycle = QLabel("Cycle: 0")
        self.lbl_pc = QLabel("PC: 0x0")
        self.lbl_stalls = QLabel("Stalls: 0")
        for l in [self.lbl_cycle, self.lbl_pc, self.lbl_stalls]:
            l.setStyleSheet("font-family: Consolas; font-size: 14px; background: #333; padding: 5px; border-radius: 3px;")
            toolbar.addWidget(l)
            
        layout.addLayout(toolbar)

        # --- MAIN SPLITTER ---
        splitter = QSplitter(Qt.Orientation.Vertical)
        
        self.pipeline_view = PipelineGraph()
        splitter.addWidget(self.pipeline_view)
        
        bottom_split = QSplitter(Qt.Orientation.Horizontal)
        
        editor_group = QGroupBox("Assembly Code")
        vbox = QVBoxLayout()
        self.editor = CodeEditor()
        self.highlighter = AssemblyHighlighter(self.editor.document())
        self.editor.setPlainText(""".text
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
""")
        vbox.addWidget(self.editor)
        editor_group.setLayout(vbox)
        bottom_split.addWidget(editor_group)
        
        self.tabs = QTabWidget()
        
        self.reg_table = QTableWidget(32, 4)
        self.reg_table.setHorizontalHeaderLabels(["Reg", "Val", "F-Reg", "Val"])
        self.reg_table.verticalHeader().setVisible(False)
        self.reg_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        for i in range(32):
            self.reg_table.setItem(i, 0, QTableWidgetItem(f"x{i}"))
            self.reg_table.setItem(i, 1, QTableWidgetItem("0"))
            self.reg_table.setItem(i, 2, QTableWidgetItem(f"f{i}"))
            self.reg_table.setItem(i, 3, QTableWidgetItem("0.0"))
            
        self.tabs.addTab(self.reg_table, "Registers")
        
        self.console = QPlainTextEdit()
        self.console.setReadOnly(True)
        self.console.setFont(QFont("Consolas", 10))
        self.console.setStyleSheet("background-color: #111; color: #0f0;")
        self.tabs.addTab(self.console, "Simulator Log")
        
        bottom_split.addWidget(self.tabs)
        bottom_split.setStretchFactor(0, 1)
        bottom_split.setStretchFactor(1, 1)
        
        splitter.addWidget(bottom_split)
        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 3)
        
        layout.addWidget(splitter)

        self.sim_process = None
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_gui)

    def log(self, msg):
        self.console.appendPlainText(msg)

    def run_sim(self):
        try:
            with open(INPUT_ASM_FILE, "w") as f:
                f.write(self.editor.toPlainText())
        except Exception as e:
            self.log(f"Error: {e}")
            return

        mode = self.combo_mode.currentIndex()
        config = {"pipeline_mode": mode, "input_program": INPUT_ASM_FILE}
        with open(CONFIG_FILE, "w") as f:
            json.dump(config, f)

        # Ensure vm_state directory exists
        if not os.path.exists("vm_state"): os.makedirs("vm_state")
        
        if self.sim_process: self.sim_process.kill()
        
        self.sim_process = QProcess()
        self.sim_process.readyReadStandardOutput.connect(self.handle_stdout)
        self.sim_process.readyReadStandardError.connect(self.handle_stderr)
        self.sim_process.finished.connect(self.sim_finished)
        
        self.console.clear()
        self.log(f"--- Starting Simulation (Mode {mode}) ---")
        
        if not os.path.exists(SIM_EXECUTABLE):
            self.log(f"ERROR: Executable {SIM_EXECUTABLE} not found.")
            return

        self.sim_process.start(SIM_EXECUTABLE, [CONFIG_FILE])
        
        self.btn_run.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.timer.start(100)

    def stop_sim(self):
        if self.sim_process:
            self.sim_process.kill()
            self.log("--- Stopped by User ---")

    def sim_finished(self):
        self.timer.stop()
        self.btn_run.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self.log("--- Simulation Finished ---")

    def handle_stdout(self):
        data = self.sim_process.readAllStandardOutput().data().decode()
        self.log(data.strip())

    def handle_stderr(self):
        data = self.sim_process.readAllStandardError().data().decode()
        self.log(f"STDERR: {data.strip()}")

    def find_file(self, filename):
        # Search in root and inside vm_state
        if os.path.exists(filename): return filename
        alt = os.path.join("vm_state", filename)
        if os.path.exists(alt): return alt
        return None

    def update_reg_cell(self, row, col, reg_name, val):
        item = self.reg_table.item(row, col)
        
        # Color change logic
        old_val = self.prev_regs.get(reg_name)
        # If value changed AND it wasn't None before (prevents green flash on startup)
        if old_val is not None and old_val != val:
            item.setBackground(QColor("#27ae60")) # Green
            item.setForeground(QColor("white"))
        else:
            item.setBackground(QColor("#1e1e1e"))
            item.setForeground(QColor("#ecf0f1"))
            
        item.setText(str(val))
        self.prev_regs[reg_name] = val

    def update_gui(self):
        # Update Pipeline
        state_file = self.find_file(STATE_FILENAME)
        if state_file:
            try:
                with open(state_file, 'r') as f:
                    data = json.load(f)
                self.lbl_cycle.setText(f"Cycle: {data.get('cycle', 0)}")
                self.lbl_pc.setText(f"PC: {data.get('program_counter_hex', '0x0')}")
                self.lbl_stalls.setText(f"Stalls: {data.get('stall_count', 0)}")
                self.pipeline_view.update_data(data.get("pipeline", {}))
            except: pass

        # Update Registers (ROBUST PARSING)
        reg_file = self.find_file(REGISTER_FILENAME)
        if reg_file:
            try:
                with open(reg_file, 'r') as f:
                    regs = json.load(f)
                
                # 1. Handle GPRs (Integer)
                if "GPR" in regs and isinstance(regs["GPR"], list):
                    for i, val in enumerate(regs["GPR"]):
                        self.update_reg_cell(i, 1, f"x{i}", val)
                elif "x0" in regs: # Map format fallback
                    for i in range(32):
                        self.update_reg_cell(i, 1, f"x{i}", regs.get(f"x{i}", 0))

                # 2. Handle FPRs (Float)
                if "FPR" in regs and isinstance(regs["FPR"], list):
                    for i, val in enumerate(regs["FPR"]):
                        self.update_reg_cell(i, 3, f"f{i}", val)
                elif "f0" in regs:
                    for i in range(32):
                        self.update_reg_cell(i, 3, f"f{i}", regs.get(f"f{i}", 0.0))
            except Exception as e:
                self.log(f"Reg Update Error: {e}")

if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    
    if hasattr(Qt.ApplicationAttribute, 'AA_EnableHighDpiScaling'):
        QApplication.setAttribute(Qt.ApplicationAttribute.AA_EnableHighDpiScaling, True)
    if hasattr(Qt.ApplicationAttribute, 'AA_UseHighDpiPixmaps'):
        QApplication.setAttribute(Qt.ApplicationAttribute.AA_UseHighDpiPixmaps, True)

    win = IDEWindow()
    win.show()
    sys.exit(app.exec())