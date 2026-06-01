#!/usr/bin/env python3
"""
GIO - IDJ Programador v2
Dos pantallas: Conexión → Programación
reTerminal 1280×720 — pantalla táctil
"""

import tkinter as tk
import asyncio
import threading
import queue
import re
from bleak import BleakScanner, BleakClient

# ─── BLE UUIDs ────────────────────────────────────────────────────────────────
CHAR_JAULA  = "0000a0b4-0000-1000-8000-00805f9b34fb"
CHAR_DOLLY  = "0000a0b6-0000-1000-8000-00805f9b34fb"
CHAR_STATUS = "0000a0b5-0000-1000-8000-00805f9b34fb"

# ─── Colores ──────────────────────────────────────────────────────────────────
BG     = "#111827"
PANEL  = "#1f2937"
CARD   = "#374151"
GREEN  = "#10b981"
RED    = "#ef4444"
BLUE   = "#3b82f6"
YELLOW = "#f59e0b"
ORANGE = "#f97316"
WHITE  = "#f9fafb"
GRAY   = "#6b7280"
GRAY2  = "#9ca3af"

W, H = 1280, 720


# ─── BLE Manager ─────────────────────────────────────────────────────────────
class BLEManager:
    def __init__(self, ui_q: queue.Queue):
        self.ui_q      = ui_q
        self.client    = None
        self.connected = False
        self.loop      = asyncio.new_event_loop()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def _push(self, tipo, datos=None):
        self.ui_q.put({"tipo": tipo, "datos": datos})

    def _go(self, coro):
        asyncio.run_coroutine_threadsafe(coro, self.loop)

    def escanear(self):           self._go(self._scan())
    def conectar(self, addr):     self._go(self._connect(addr))
    def desconectar(self):        self._go(self._disconnect())
    def leer(self):               self._go(self._write(CHAR_JAULA, "READ"))
    def programar(self, j, d):    self._go(self._programar(j, d))

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
            self.client = BleakClient(
                addr,
                disconnected_callback=lambda c: self._push("disconnected", None))
            await self.client.connect()
            self.connected = True
            await self.client.start_notify(CHAR_STATUS, self._on_notify)
            self._push("connected", addr)
            await asyncio.sleep(0.8)
            await self._write(CHAR_JAULA, "READ")
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

    async def _write(self, uuid, value: str):
        if not self.connected:
            self._push("status", ("No conectado", RED))
            return
        try:
            await self.client.write_gatt_char(uuid, value.encode())
        except Exception as e:
            self._push("status", (f"Error escritura: {e}", RED))

    async def _programar(self, jaula: int, dolly: int):
        self._push("status", ("Programando...", YELLOW))
        await self._write(CHAR_DOLLY, str(dolly))
        await asyncio.sleep(0.3)
        await self._write(CHAR_JAULA, str(jaula))

    def _on_notify(self, sender, data: bytearray):
        self._push("notify", data.decode("utf-8", errors="replace").strip())


# ─── Aplicación ───────────────────────────────────────────────────────────────
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("GIO - IDJ Programador")
        self.geometry(f"{W}x{H}+0+0")
        self.resizable(False, False)
        self.configure(bg=BG)

        self.ui_q    = queue.Queue()
        self.ble     = BLEManager(self.ui_q)
        self.devices = []
        self.sel_dev = None
        self.active  = "jaula"   # campo activo en pantalla 2

        # Variables compartidas entre pantallas
        self.var_jaula     = tk.StringVar(value="")
        self.var_dolly     = tk.StringVar(value="")
        self.var_cur_jaula = tk.StringVar(value="—")
        self.var_cur_dolly = tk.StringVar(value="—")
        self.var_status    = tk.StringVar(value="Presiona ESCANEAR para buscar el IDJ Programador")
        self.var_ble_info  = tk.StringVar(value="Sin conexión")

        self._build()
        self._show("conn")
        self._poll()

    # ─── Navegación ───────────────────────────────────────────────────────────
    def _build(self):
        self.scr_conn = tk.Frame(self, bg=BG)
        self.scr_prog = tk.Frame(self, bg=BG)
        for s in (self.scr_conn, self.scr_prog):
            s.place(x=0, y=0, width=W, height=H)
        self._build_conn(self.scr_conn)
        self._build_prog(self.scr_prog)

    def _show(self, name):
        if name == "conn":
            self.scr_conn.tkraise()
        else:
            self.scr_prog.tkraise()

    # =========================================================================
    # PANTALLA 1 — CONEXIÓN BLE
    # =========================================================================
    def _build_conn(self, p):
        # Header
        hdr = tk.Frame(p, bg=PANEL, height=65)
        hdr.place(x=0, y=0, width=W, height=65)
        tk.Label(hdr, text="GIO  ·  IDJ PROGRAMADOR",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 22, "bold")).place(x=30, y=16)
        tk.Label(hdr, text="Pantalla de Conexión",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 20)).place(x=520, y=22)

        # Centro — tarjeta de conexión
        card = tk.Frame(p, bg=PANEL)
        card.place(x=240, y=85, width=800, height=580)

        tk.Label(card,
                 text="Conectar al IDJ Programador",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 20, "bold")).place(x=0, y=22, width=800)

        tk.Label(card,
                 text="Asegúrate de que el IDJ Programador esté encendido y cerca",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 18)).place(x=0, y=64, width=800)

        # Botón escanear
        self.btn_scan = self._btn(card, "⟳   ESCANEAR DISPOSITIVOS",
                                  BLUE, self._on_scan,
                                  x=200, y=115, w=400, h=80,
                                  font=("Arial", 17, "bold"))

        # Lista de dispositivos
        tk.Label(card, text="Dispositivos IDJ encontrados:",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 15, "bold")).place(x=50, y=215)

        lf = tk.Frame(card, bg=CARD)
        lf.place(x=50, y=245, width=700, height=120)
        self.lbox = tk.Listbox(lf,
                               bg=CARD, fg=WHITE,
                               selectbackground=BLUE,
                               selectforeground=WHITE,
                               font=("Arial", 16),
                               height=5,
                               borderwidth=0,
                               highlightthickness=0,
                               activestyle="none")
        self.lbox.pack(fill="both", expand=True, padx=6, pady=6)
        self.lbox.bind("<<ListboxSelect>>", self._on_select)

        # Botón conectar
        self.btn_conn = self._btn(card, "CONECTAR  →",
                                  GREEN, self._on_connect,
                                  x=200, y=422, w=400, h=80,
                                  font=("Arial", 20, "bold"))

        # Status bar
        sb = tk.Frame(p, bg=PANEL, height=55)
        sb.place(x=0, y=H - 55, width=W, height=55)
        tk.Label(sb, text="Estado:", bg=PANEL, fg=GRAY,
                 font=("Arial", 13)).place(x=20, y=16)
        self.lbl_s1 = tk.Label(sb, textvariable=self.var_status,
                                bg=PANEL, fg=GREEN,
                                font=("Arial", 13, "bold"))
        self.lbl_s1.place(x=90, y=16)

    # =========================================================================
    # PANTALLA 2 — LECTURA Y PROGRAMACIÓN
    # =========================================================================
    def _build_prog(self, p):
        # Header
        hdr = tk.Frame(p, bg=PANEL, height=65)
        hdr.place(x=0, y=0, width=W, height=65)

        self._btn(hdr, "←  VOLVER",
                  CARD, self._on_volver,
                  x=15, y=12, w=140, h=42,
                  font=("Arial", 13, "bold"))

        tk.Label(hdr, text="GIO  ·  IDJ PROGRAMADOR",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 19, "bold")).place(x=175, y=18)

        self.dot2 = tk.Label(hdr, text="●", bg=PANEL, fg=GREEN,
                             font=("Arial", 14))
        self.dot2.place(x=W - 280, y=22)

        tk.Label(hdr, textvariable=self.var_ble_info,
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 12)).place(x=W - 260, y=22)

        # ── Panel izquierdo (estado + formulario) ─────────────────────────────
        LWIDTH = 490
        body_y = 70
        body_h = H - 70 - 55

        # Card: estado actual
        st_card = tk.Frame(p, bg=PANEL)
        st_card.place(x=10, y=body_y + 8, width=LWIDTH, height=205)

        tk.Label(st_card, text="ESCLAVO CONECTADO — ESTADO ACTUAL",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=12, y=10)

        # Jaula actual
        jcard = tk.Frame(st_card, bg=CARD)
        jcard.place(x=12, y=35, width=216, height=148)
        tk.Label(jcard, text="Jaula actual",
                 bg=CARD, fg=GRAY2, font=("Arial", 14)).place(x=10, y=10)
        tk.Label(jcard, textvariable=self.var_cur_jaula,
                 bg=CARD, fg=GREEN,
                 font=("Arial", 24, "bold"),
                 wraplength=195).place(x=10, y=52)

        # Dolly actual
        dcard = tk.Frame(st_card, bg=CARD)
        dcard.place(x=242, y=35, width=216, height=148)
        tk.Label(dcard, text="Dolly actual",
                 bg=CARD, fg=GRAY2, font=("Arial", 14)).place(x=10, y=10)
        tk.Label(dcard, textvariable=self.var_cur_dolly,
                 bg=CARD, fg=BLUE,
                 font=("Arial", 24, "bold"),
                 wraplength=195).place(x=10, y=52)

        # Botón leer
        self._btn(st_card, "↻  LEER",
                  BLUE, self._on_leer,
                  x=360, y=10, w=118, h=18)

        tk.Frame(p, bg=CARD, height=2).place(
            x=10, y=body_y + 220, width=LWIDTH, height=2)

        # Formulario de programación
        fy = body_y + 232

        tk.Label(p, text="PROGRAMAR NUEVO NÚMERO",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 12, "bold")).place(x=20, y=fy)

        # Campo Jaula
        tk.Label(p, text="Número de Jaula  (T0603-xxxx)  *requerido",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 12)).place(x=20, y=fy + 30)

        self.fr_jaula = tk.Frame(p, bg=BLUE, padx=3, pady=3)
        self.fr_jaula.place(x=20, y=fy + 55, width=LWIDTH - 20, height=78)
        self.lbl_j = tk.Label(self.fr_jaula,
                              textvariable=self.var_jaula,
                              bg=CARD, fg=WHITE,
                              font=("Arial", 36, "bold"),
                              anchor="e", padx=14,
                              cursor="hand2")
        self.lbl_j.pack(fill="both", expand=True)
        self.lbl_j.bind("<Button-1>", lambda e: self._set_active("jaula"))

        # Campo Dolly
        tk.Label(p, text="Número de Dolly  (T0605-xxxx)  — 0 si no aplica",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 12)).place(x=20, y=fy + 148)

        self.fr_dolly = tk.Frame(p, bg=GRAY, padx=3, pady=3)
        self.fr_dolly.place(x=20, y=fy + 173, width=LWIDTH - 20, height=78)
        self.lbl_d = tk.Label(self.fr_dolly,
                              textvariable=self.var_dolly,
                              bg=CARD, fg=WHITE,
                              font=("Arial", 36, "bold"),
                              anchor="e", padx=14,
                              cursor="hand2")
        self.lbl_d.pack(fill="both", expand=True)
        self.lbl_d.bind("<Button-1>", lambda e: self._set_active("dolly"))

        # Indicador campo activo
        self.lbl_act = tk.Label(p,
                                text="▲ Toca un campo para editarlo",
                                bg=BG, fg=GRAY,
                                font=("Arial", 12))
        self.lbl_act.place(x=20, y=fy + 262)

        # Botones
        self.btn_prog = self._btn(p, "▶  PROGRAMAR",
                                  ORANGE, self._on_programar,
                                  x=20, y=fy + 290, w=290, h=70,
                                  font=("Arial", 17, "bold"))

        self._btn(p, "✕  LIMPIAR",
                  CARD, self._on_limpiar,
                  x=325, y=fy + 290, w=165, h=70,
                  font=("Arial", 14, "bold"))

        # Separador vertical
        tk.Frame(p, bg=CARD, width=2).place(
            x=LWIDTH + 12, y=body_y, width=2, height=body_h)

        # ── Panel derecho — Numpad grande ─────────────────────────────────────
        self._numpad_grande(p, x=LWIDTH + 22, y=body_y,
                            w=W - LWIDTH - 24, h=body_h)

        # Status bar
        sb = tk.Frame(p, bg=PANEL, height=55)
        sb.place(x=0, y=H - 55, width=W, height=55)
        tk.Label(sb, text="Estado:", bg=PANEL, fg=GRAY,
                 font=("Arial", 13)).place(x=20, y=16)
        self.lbl_s2 = tk.Label(sb, textvariable=self.var_status,
                                bg=PANEL, fg=GREEN,
                                font=("Arial", 13, "bold"))
        self.lbl_s2.place(x=90, y=16)

    def _numpad_grande(self, parent, x, y, w, h):
        BW  = 192   # ancho botón
        BH  = 128   # alto botón
        GAP = 10    # espacio entre botones

        # Centrar el numpad en el panel derecho
        total_w = 3 * BW + 2 * GAP
        total_h = 4 * BH + 3 * GAP
        ox = x + (w - total_w) // 2
        oy = y + (h - total_h) // 2 + 10

        tk.Label(parent, text="TECLADO NUMÉRICO",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 12, "bold")).place(
                     x=ox, y=oy - 32, width=total_w)

        keys = [
            ("7", 0, 0, CARD),  ("8", 0, 1, CARD),  ("9", 0, 2, CARD),
            ("4", 1, 0, CARD),  ("5", 1, 1, CARD),  ("6", 1, 2, CARD),
            ("1", 2, 0, CARD),  ("2", 2, 1, CARD),  ("3", 2, 2, CARD),
            ("←", 3, 0, YELLOW), ("0", 3, 1, CARD), ("✓", 3, 2, GREEN),
        ]

        for (txt, row, col, color) in keys:
            bx = ox + col * (BW + GAP)
            by = oy + row * (BH + GAP)

            if txt == "←":
                cmd = self._on_back
            elif txt == "✓":
                cmd = self._on_ok
            else:
                cmd = lambda t=txt: self._on_digit(t)

            tk.Button(parent,
                      text=txt,
                      bg=color, fg=WHITE,
                      font=("Arial", 28, "bold"),
                      relief="flat",
                      activebackground=BLUE,
                      activeforeground=WHITE,
                      command=cmd).place(x=bx, y=by, width=BW, height=BH)

    # ─── Helper botón ─────────────────────────────────────────────────────────
    def _btn(self, parent, text, color, cmd, x, y, w, h, font=None):
        if font is None:
            font = ("Arial", 13, "bold")
        b = tk.Button(parent, text=text,
                      bg=color, fg=WHITE, font=font,
                      relief="flat",
                      activebackground=WHITE, activeforeground=BG,
                      command=cmd)
        b.place(x=x, y=y, width=w, height=h)
        return b

    def _set_active(self, field):
        self.active = field
        if field == "jaula":
            self.fr_jaula.config(bg=BLUE)
            self.fr_dolly.config(bg=GRAY)
            self.lbl_act.config(
                text="▲ Editando: JAULA  (T0603-XXXX)", fg=BLUE)
        else:
            self.fr_dolly.config(bg=BLUE)
            self.fr_jaula.config(bg=GRAY)
            self.lbl_act.config(
                text="▲ Editando: DOLLY  (T0605-XXXX)  —  deja en 0 si no aplica",
                fg=BLUE)

    def _set_status(self, msg, color=GREEN):
        self.var_status.set(msg)
        self.lbl_s1.config(fg=color)
        self.lbl_s2.config(fg=color)

    # ─── Numpad callbacks ─────────────────────────────────────────────────────
    def _on_digit(self, d):
        v = self.var_jaula if self.active == "jaula" else self.var_dolly
        if len(v.get()) < 4:
            v.set(v.get() + d)

    def _on_back(self):
        v = self.var_jaula if self.active == "jaula" else self.var_dolly
        v.set(v.get()[:-1])

    def _on_ok(self):
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

    def _on_volver(self):
        self.ble.desconectar()
        self._show("conn")

    def _on_leer(self):
        self.ble.leer()
        self._set_status("Leyendo EEPROM...", YELLOW)

    def _on_programar(self):
        j_str = self.var_jaula.get().strip()
        if not j_str or not j_str.isdigit() or not 1 <= int(j_str) <= 9999:
            self._set_status("⚠  Ingresa un número de jaula válido (1-9999)", YELLOW)
            return
        d_str = self.var_dolly.get().strip()
        d = int(d_str) if d_str.isdigit() and int(d_str) > 0 else 0
        self.ble.programar(int(j_str), d)

    # ─── Parser notificaciones ────────────────────────────────────────────────
    def _parse_notify(self, msg: str):
        if msg.startswith("READ:"):
            if "VIRGEN" in msg:
                self.var_cur_jaula.set("Sin programar")
                self.var_cur_dolly.set("—")
                self._set_status("Esclavo sin programar aún", YELLOW)
            else:
                m = re.match(r"READ:jaula=(\d+),dolly=(\w+)", msg)
                if m:
                    self.var_cur_jaula.set(f"T0603-{int(m.group(1)):04d}")
                    dv = m.group(2)
                    self.var_cur_dolly.set(
                        "Sin dolly" if dv in ("NONE", "0")
                        else f"T0605-{int(dv):04d}")
                    self._set_status("EEPROM leída correctamente", GREEN)
            return

        if msg.startswith("READ_ERROR:"):
            err = msg.split(":", 1)[1].replace("_", " ")
            self.var_cur_jaula.set("Error")
            self.var_cur_dolly.set("—")
            self._set_status(f"⚠  {err}", RED)
            return

        if msg.startswith("OK!"):
            self._set_status(f"✅  {msg}", GREEN)
            self.after(900, self._on_leer)
            return

        if msg.startswith("ERROR:"):
            self._set_status(f"❌  {msg.split(':', 1)[1]}", RED)
            return

        self._set_status(msg, GRAY2)

    # ─── Queue polling ────────────────────────────────────────────────────────
    def _poll(self):
        try:
            while True:
                m  = self.ui_q.get_nowait()
                tp = m["tipo"]
                dt = m["datos"]

                if tp == "scan_ok":
                    self.devices = dt
                    self.lbox.delete(0, "end")
                    if dt:
                        for d in dt:
                            self.lbox.insert(
                                "end", f"   {d.name}     ({d.address})")
                        self.lbox.selection_set(0)
                        self.sel_dev = dt[0]
                        self._set_status(
                            f"{len(dt)} dispositivo(s) IDJ encontrado(s)"
                            " — selecciona y presiona CONECTAR", GREEN)
                    else:
                        self._set_status(
                            "No se encontraron dispositivos IDJ — intenta de nuevo",
                            YELLOW)

                elif tp == "connected":
                    self.dot2.config(fg=GREEN)
                    self.var_ble_info.set(
                        f"Conectado  •  {self.sel_dev.name if self.sel_dev else dt}")
                    self._set_status(
                        "Conectado — leyendo estado del esclavo...", GREEN)
                    self.btn_prog.config(state="normal")
                    self._set_active("jaula")
                    self._show("prog")   # ← navegar a pantalla 2

                elif tp == "disconnected":
                    self.dot2.config(fg=RED)
                    self.var_ble_info.set("Sin conexión")
                    self.var_cur_jaula.set("—")
                    self.var_cur_dolly.set("—")
                    self.btn_prog.config(state="disabled")
                    self._set_status("Desconectado", RED)
                    self._show("conn")   # ← volver a pantalla 1

                elif tp == "notify":
                    self._parse_notify(dt)

                elif tp == "status":
                    self._set_status(*dt)

        except queue.Empty:
            pass

        self.after(100, self._poll)


if __name__ == "__main__":
    app = App()
    app.mainloop()
