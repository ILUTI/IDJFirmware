#!/usr/bin/env python3
"""
GIO - IDJ Programador v2
App táctil para programar esclavos IDJ desde el reTerminal (1280×720)
"""

import tkinter as tk
import asyncio
import threading
import queue
import re
from bleak import BleakScanner, BleakClient

# ─── BLE UUIDs ────────────────────────────────────────────────────────────────
CHAR_JAULA  = "0000a0b4-0000-1000-8000-00805f9b34fb"  # Write número / "READ"
CHAR_DOLLY  = "0000a0b6-0000-1000-8000-00805f9b34fb"  # Write número dolly
CHAR_STATUS = "0000a0b5-0000-1000-8000-00805f9b34fb"  # Notify respuesta

# ─── Paleta ───────────────────────────────────────────────────────────────────
BG      = "#111827"
PANEL   = "#1f2937"
CARD    = "#374151"
GREEN   = "#10b981"
RED     = "#ef4444"
BLUE    = "#3b82f6"
YELLOW  = "#f59e0b"
ORANGE  = "#f97316"
WHITE   = "#f9fafb"
GRAY    = "#6b7280"
GRAY2   = "#9ca3af"

# ─── BLE Manager (hilo asyncio separado) ─────────────────────────────────────
class BLEManager:
    def __init__(self, ui_q: queue.Queue):
        self.ui_q    = ui_q
        self.client  = None
        self.connected = False
        self.loop    = asyncio.new_event_loop()
        t = threading.Thread(target=self._run_loop, daemon=True)
        t.start()

    def _run_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def _push(self, tipo, datos=None):
        self.ui_q.put({"tipo": tipo, "datos": datos})

    def _go(self, coro):
        asyncio.run_coroutine_threadsafe(coro, self.loop)

    # ── API pública ───────────────────────────────────────────────────────────
    def escanear(self):      self._go(self._scan())
    def conectar(self, addr): self._go(self._connect(addr))
    def desconectar(self):   self._go(self._disconnect())
    def leer(self):          self._go(self._write(CHAR_JAULA, "READ"))
    def programar(self, j, d): self._go(self._programar(j, d))

    # ── Coroutines ────────────────────────────────────────────────────────────
    async def _scan(self):
        self._push("status", ("Escaneando...", YELLOW))
        try:
            devices = await BleakScanner.discover(timeout=5.0)
            idj = [d for d in devices if d.name and "IDJ" in d.name.upper()]
            self._push("scan_ok", idj)
        except Exception as e:
            self._push("status", (f"Error escaneo: {e}", RED))

    async def _connect(self, addr):
        self._push("status", ("Conectando...", YELLOW))
        try:
            self.client = BleakClient(addr,
                disconnected_callback=lambda c: self._push("disconnected", None))
            await self.client.connect()
            self.connected = True
            await self.client.start_notify(CHAR_STATUS, self._on_notify)
            self._push("connected", addr)
            await asyncio.sleep(0.8)
            await self._write(CHAR_JAULA, "READ")   # Auto-leer al conectar
        except Exception as e:
            self.connected = False
            self._push("status", (f"Error: {e}", RED))
            self._push("disconnected", None)

    async def _disconnect(self):
        if self.client:
            try: await self.client.disconnect()
            except: pass
        self.connected = False
        self._push("disconnected", None)

    async def _write(self, char_uuid, value: str):
        if not self.connected:
            self._push("status", ("No conectado", RED))
            return
        try:
            await self.client.write_gatt_char(
                char_uuid, value.encode("utf-8"))
        except Exception as e:
            self._push("status", (f"Error escritura: {e}", RED))

    async def _programar(self, jaula: int, dolly: int):
        if not self.connected:
            self._push("status", ("No conectado", RED))
            return
        self._push("status", ("Programando...", YELLOW))
        await self._write(CHAR_DOLLY, str(dolly))
        await asyncio.sleep(0.3)
        await self._write(CHAR_JAULA, str(jaula))

    def _on_notify(self, sender, data: bytearray):
        self._push("notify", data.decode("utf-8", errors="replace").strip())


# ─── Aplicación principal ─────────────────────────────────────────────────────
class App(tk.Tk):
    W, H = 1280, 720

    def __init__(self):
        super().__init__()
        self.title("GIO - IDJ Programador")
        self.geometry(f"{self.W}x{self.H}+0+0")
        self.resizable(False, False)
        self.configure(bg=BG)

        self.ui_q    = queue.Queue()
        self.ble     = BLEManager(self.ui_q)
        self.devices = []          # lista BLE encontrada
        self.sel_dev = None        # dispositivo seleccionado
        self.active  = "jaula"     # campo activo del numpad

        # Variables tkinter
        self.var_jaula     = tk.StringVar(value="")
        self.var_dolly     = tk.StringVar(value="")
        self.var_cur_jaula = tk.StringVar(value="—")
        self.var_cur_dolly = tk.StringVar(value="—")
        self.var_ble_txt   = tk.StringVar(value="Desconectado")
        self.var_status    = tk.StringVar(value="Listo — escanea y conecta el IDJ Programador")

        self._ui()
        self._poll()

    # ─── Construcción de UI ───────────────────────────────────────────────────
    def _ui(self):
        self._header()
        body = tk.Frame(self, bg=BG)
        body.place(x=0, y=55, width=self.W, height=self.H - 55 - 52)
        self._left(body)
        tk.Frame(body, bg=CARD, width=2).place(x=310, y=0, width=2, height=self.H - 107)
        self._right(body)
        self._statusbar()

    def _header(self):
        f = tk.Frame(self, bg=PANEL, height=55)
        f.place(x=0, y=0, width=self.W, height=55)

        tk.Label(f, text="GIO  ·  IDJ PROGRAMADOR",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 19, "bold")).place(x=20, y=12)

        self.dot = tk.Label(f, text="●", bg=PANEL, fg=RED, font=("Arial", 14))
        self.dot.place(x=self.W - 220, y=16)

        tk.Label(f, textvariable=self.var_ble_txt,
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 12)).place(x=self.W - 200, y=18)

    def _left(self, parent):
        # Título
        tk.Label(parent, text="CONEXIÓN BLE",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 11, "bold")).place(x=15, y=12)

        # Botón escanear
        self._btn(parent, "⟳  ESCANEAR", BLUE, self._on_scan,
                  x=15, y=40, w=280, h=52)

        # Lista de dispositivos
        tk.Label(parent, text="Dispositivos IDJ encontrados:",
                 bg=BG, fg=GRAY,
                 font=("Arial", 10)).place(x=15, y=103)

        lf = tk.Frame(parent, bg=CARD)
        lf.place(x=15, y=122, width=280, height=130)
        self.lbox = tk.Listbox(lf,
                               bg=CARD, fg=WHITE,
                               selectbackground=BLUE,
                               selectforeground=WHITE,
                               font=("Arial", 12),
                               height=4,
                               borderwidth=0,
                               highlightthickness=0,
                               activestyle="none")
        self.lbox.pack(fill="both", expand=True, padx=4, pady=4)
        self.lbox.bind("<<ListboxSelect>>", self._on_select)

        # Conectar / Desconectar
        self.btn_conn = self._btn(parent, "CONECTAR", GREEN, self._on_connect,
                                  x=15, y=265, w=280, h=58)
        self.btn_disc = self._btn(parent, "DESCONECTAR", RED, self._on_disconnect,
                                  x=15, y=332, w=280, h=45)
        self.btn_disc.config(state="disabled")

        # Info de ayuda
        tk.Label(parent,
                 text="1. Escanea\n2. Selecciona IDJ-PROG\n3. Conecta",
                 bg=BG, fg=GRAY,
                 font=("Arial", 10),
                 justify="left").place(x=15, y=395)

    def _right(self, parent):
        rx = 318   # x base del panel derecho

        # ── Card: estado actual ───────────────────────────────────────────────
        card = tk.Frame(parent, bg=PANEL)
        card.place(x=rx, y=8, width=950, height=168)

        tk.Label(card, text="ESCLAVO ACTUALMENTE CONECTADO",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=12, y=10)

        # Jaula actual
        jf = tk.Frame(card, bg=CARD)
        jf.place(x=12, y=36, width=290, height=118)
        tk.Label(jf, text="Jaula programada",
                 bg=CARD, fg=GRAY2, font=("Arial", 11)).place(x=10, y=10)
        tk.Label(jf, textvariable=self.var_cur_jaula,
                 bg=CARD, fg=GREEN,
                 font=("Arial", 28, "bold")).place(x=10, y=40)

        # Dolly actual
        df = tk.Frame(card, bg=CARD)
        df.place(x=315, y=36, width=290, height=118)
        tk.Label(df, text="Dolly programado",
                 bg=CARD, fg=GRAY2, font=("Arial", 11)).place(x=10, y=10)
        tk.Label(df, textvariable=self.var_cur_dolly,
                 bg=CARD, fg=BLUE,
                 font=("Arial", 28, "bold")).place(x=10, y=40)

        # Botón leer
        self._btn(card, "↻ LEER", CARD, self._on_leer,
                  x=836, y=55, w=102, h=48)

        tk.Frame(parent, bg=CARD, height=2).place(x=rx, y=180, width=950, height=2)

        # ── Área de programación ──────────────────────────────────────────────
        tk.Label(parent, text="PROGRAMAR NUEVO",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 11, "bold")).place(x=rx + 12, y=192)

        # Campo Jaula
        tk.Label(parent, text="Número de Jaula  (T0603-xxxx)  *requerido",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 11)).place(x=rx + 12, y=220)

        self.frame_jaula = tk.Frame(parent, bg=BLUE, padx=3, pady=3)
        self.frame_jaula.place(x=rx + 12, y=244, width=450, height=72)
        self.lbl_jaula = tk.Label(self.frame_jaula,
                                  textvariable=self.var_jaula,
                                  bg=CARD, fg=WHITE,
                                  font=("Arial", 30, "bold"),
                                  anchor="e", padx=12,
                                  cursor="hand2")
        self.lbl_jaula.pack(fill="both", expand=True)
        self.lbl_jaula.bind("<Button-1>", lambda e: self._set_active("jaula"))

        # Campo Dolly
        tk.Label(parent, text="Número de Dolly  (T0605-xxxx)  opcional — 0 si no aplica",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 11)).place(x=rx + 12, y=326)

        self.frame_dolly = tk.Frame(parent, bg=GRAY, padx=3, pady=3)
        self.frame_dolly.place(x=rx + 12, y=350, width=450, height=72)
        self.lbl_dolly = tk.Label(self.frame_dolly,
                                  textvariable=self.var_dolly,
                                  bg=CARD, fg=WHITE,
                                  font=("Arial", 30, "bold"),
                                  anchor="e", padx=12,
                                  cursor="hand2")
        self.lbl_dolly.pack(fill="both", expand=True)
        self.lbl_dolly.bind("<Button-1>", lambda e: self._set_active("dolly"))

        # Indicador campo activo
        self.lbl_active = tk.Label(parent,
                                   text="▲ Toca un campo para editarlo",
                                   bg=BG, fg=GRAY,
                                   font=("Arial", 11))
        self.lbl_active.place(x=rx + 12, y=432)

        # Botones acción
        self.btn_prog = self._btn(parent, "▶  PROGRAMAR", ORANGE, self._on_programar,
                                  x=rx + 12, y=460, w=280, h=65,
                                  font=("Arial", 16, "bold"))
        self.btn_prog.config(state="disabled")

        self._btn(parent, "✕  LIMPIAR", CARD, self._on_limpiar,
                  x=rx + 305, y=460, w=157, h=65)

        # ── Numpad ────────────────────────────────────────────────────────────
        self._numpad(parent, x=rx + 490, y=192)

    def _numpad(self, parent, x, y):
        tk.Label(parent, text="TECLADO NUMÉRICO",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=x + 20, y=y)

        keys = [
            ("7", 0, 0, CARD),  ("8", 0, 1, CARD),  ("9", 0, 2, CARD),
            ("4", 1, 0, CARD),  ("5", 1, 1, CARD),  ("6", 1, 2, CARD),
            ("1", 2, 0, CARD),  ("2", 2, 1, CARD),  ("3", 2, 2, CARD),
            ("←", 3, 0, YELLOW), ("0", 3, 1, CARD), ("✓", 3, 2, GREEN),
        ]
        BW, BH, GAP = 120, 78, 6

        for (txt, row, col, color) in keys:
            bx = x + col * (BW + GAP)
            by = y + 28 + row * (BH + GAP)

            if txt == "←":
                cmd = self._on_back
            elif txt == "✓":
                cmd = self._on_ok
            else:
                cmd = lambda t=txt: self._on_digit(t)

            btn = tk.Button(parent,
                            text=txt,
                            bg=color, fg=WHITE,
                            font=("Arial", 22, "bold"),
                            relief="flat",
                            activebackground=BLUE,
                            activeforeground=WHITE,
                            command=cmd)
            btn.place(x=bx, y=by, width=BW, height=BH)

    def _statusbar(self):
        f = tk.Frame(self, bg=PANEL, height=52)
        f.place(x=0, y=self.H - 52, width=self.W, height=52)

        tk.Label(f, text="Estado:", bg=PANEL, fg=GRAY,
                 font=("Arial", 12)).place(x=15, y=16)

        self.lbl_status = tk.Label(f,
                                   textvariable=self.var_status,
                                   bg=PANEL, fg=GREEN,
                                   font=("Arial", 12, "bold"))
        self.lbl_status.place(x=80, y=16)

    # ─── Helpers ─────────────────────────────────────────────────────────────
    def _btn(self, parent, text, color, cmd, x, y, w, h, font=None):
        if font is None:
            font = ("Arial", 13, "bold")
        b = tk.Button(parent,
                      text=text,
                      bg=color, fg=WHITE,
                      font=font,
                      relief="flat",
                      activebackground=WHITE,
                      activeforeground=BG,
                      command=cmd)
        b.place(x=x, y=y, width=w, height=h)
        return b

    def _set_active(self, field):
        self.active = field
        if field == "jaula":
            self.frame_jaula.config(bg=BLUE)
            self.frame_dolly.config(bg=GRAY)
            self.lbl_active.config(text="▲ Editando: JAULA  (T0603-XXXX)", fg=BLUE)
        else:
            self.frame_dolly.config(bg=BLUE)
            self.frame_jaula.config(bg=GRAY)
            self.lbl_active.config(text="▲ Editando: DOLLY  (T0605-XXXX) — deja en 0 si no aplica", fg=BLUE)

    def _set_status(self, msg, color=GREEN):
        self.var_status.set(msg)
        self.lbl_status.config(fg=color)

    # ─── Numpad callbacks ─────────────────────────────────────────────────────
    def _on_digit(self, d):
        v = self.var_jaula if self.active == "jaula" else self.var_dolly
        if len(v.get()) < 4:
            v.set(v.get() + d)

    def _on_back(self):
        v = self.var_jaula if self.active == "jaula" else self.var_dolly
        v.set(v.get()[:-1])

    def _on_ok(self):
        # ✓ en jaula → pasar a dolly; ✓ en dolly → programar
        if self.active == "jaula":
            self._set_active("dolly")
        else:
            self._on_programar()

    def _on_limpiar(self):
        self.var_jaula.set("")
        self.var_dolly.set("")
        self._set_active("jaula")

    # ─── BLE callbacks ────────────────────────────────────────────────────────
    def _on_scan(self):
        self.lbox.delete(0, "end")
        self.devices = []
        self.ble.escanear()

    def _on_select(self, _):
        sel = self.lbox.curselection()
        if sel:
            self.sel_dev = self.devices[sel[0]]

    def _on_connect(self):
        if self.sel_dev:
            self.ble.conectar(self.sel_dev.address)
        else:
            self._set_status("Selecciona un dispositivo de la lista", YELLOW)

    def _on_disconnect(self):
        self.ble.desconectar()

    def _on_leer(self):
        self.ble.leer()
        self._set_status("Leyendo EEPROM del esclavo...", YELLOW)

    def _on_programar(self):
        j_str = self.var_jaula.get().strip()
        if not j_str or not j_str.isdigit():
            self._set_status("⚠  Ingresa un número de jaula válido", YELLOW)
            return
        j = int(j_str)
        if not 1 <= j <= 9999:
            self._set_status("⚠  Jaula debe estar entre 1 y 9999", YELLOW)
            return
        d_str = self.var_dolly.get().strip()
        d = int(d_str) if d_str.isdigit() and int(d_str) > 0 else 0
        self.ble.programar(j, d)

    # ─── Parser de notificaciones BLE ─────────────────────────────────────────
    def _parse_notify(self, msg: str):
        # READ:jaula=230,dolly=45  |  READ:jaula=230,dolly=NONE  |  READ:VIRGEN
        if msg.startswith("READ:"):
            if "VIRGEN" in msg:
                self.var_cur_jaula.set("Sin programar")
                self.var_cur_dolly.set("—")
                self._set_status("Esclavo sin programar", YELLOW)
            else:
                m = re.match(r"READ:jaula=(\d+),dolly=(\w+)", msg)
                if m:
                    self.var_cur_jaula.set(f"T0603-{int(m.group(1)):04d}")
                    dolly_val = m.group(2)
                    if dolly_val in ("NONE", "0"):
                        self.var_cur_dolly.set("Sin dolly")
                    else:
                        self.var_cur_dolly.set(f"T0605-{int(dolly_val):04d}")
                    self._set_status("EEPROM leída correctamente", GREEN)
            return

        if msg.startswith("READ_ERROR:"):
            err = msg.split(":", 1)[1].replace("_", " ")
            self.var_cur_jaula.set("Error")
            self.var_cur_dolly.set("—")
            self._set_status(f"⚠  {err}", RED)
            return

        # OK![J:NUEVA D:NONE]->[J:#230 D:#45]
        if msg.startswith("OK!"):
            self._set_status(f"✅  {msg}", GREEN)
            self.after(900, self._on_leer)   # Auto-leer para confirmar
            return

        if msg.startswith("ERROR:"):
            self._set_status(f"❌  {msg.split(':', 1)[1]}", RED)
            return

        self._set_status(msg, GRAY2)

    # ─── Queue polling ────────────────────────────────────────────────────────
    def _poll(self):
        try:
            while True:
                m = self.ui_q.get_nowait()
                tipo, datos = m["tipo"], m["datos"]

                if tipo == "scan_ok":
                    self.devices = datos
                    self.lbox.delete(0, "end")
                    if datos:
                        for d in datos:
                            self.lbox.insert("end", f"  {d.name}   ({d.address[-5:]})")
                        self.lbox.selection_set(0)
                        self.sel_dev = datos[0]
                        self._set_status(f"{len(datos)} dispositivo(s) encontrado(s)", GREEN)
                    else:
                        self._set_status("No se encontraron dispositivos IDJ", YELLOW)

                elif tipo == "connected":
                    self.dot.config(fg=GREEN)
                    self.var_ble_txt.set("Conectado")
                    self.btn_conn.config(state="disabled")
                    self.btn_disc.config(state="normal")
                    self.btn_prog.config(state="normal")
                    self._set_status("Conectado — leyendo estado del esclavo...", GREEN)
                    self.after(300, lambda: self._set_active("jaula"))

                elif tipo == "disconnected":
                    self.dot.config(fg=RED)
                    self.var_ble_txt.set("Desconectado")
                    self.btn_conn.config(state="normal")
                    self.btn_disc.config(state="disabled")
                    self.btn_prog.config(state="disabled")
                    self.var_cur_jaula.set("—")
                    self.var_cur_dolly.set("—")
                    self._set_status("Desconectado del IDJ Programador", RED)

                elif tipo == "notify":
                    self._parse_notify(datos)

                elif tipo == "status":
                    msg_txt, color = datos
                    self._set_status(msg_txt, color)

        except queue.Empty:
            pass

        self.after(100, self._poll)


# ─── Entry point ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    app = App()
    app.mainloop()
