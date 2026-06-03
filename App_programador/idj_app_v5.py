#!/usr/bin/env python3
"""
GIO - IDJ Programador v5
reTerminal 1280x720 — selección desde API, pantalla táctil
"""

import tkinter as tk
import asyncio
import threading
import queue
import re
import json
import subprocess
import urllib.request
import urllib.error
from bleak import BleakScanner, BleakClient

# ─── APIs ─────────────────────────────────────────────────────────────────────
API_JAULAS  = ("https://mcubackend.launion.com.gt/api/flujo/412/ejecutar/"
               "?public_key=YKlo4Q8yNNzVsgTGfJFUE5p0ZocgGmkL&responseIdentifier=1")
API_DOLLIES = ("https://mcubackend.launion.com.gt/api/flujo/411/ejecutar/"
               "?public_key=YKlo4Q8yNNzVsgTGfJFUE5p0ZocgGmkL&responseIdentifier=1")

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
TEAL   = "#0891b2"
PURPLE = "#7c3aed"

W, H = 1280, 720


# ─── BLE Manager ─────────────────────────────────────────────────────────────
class BLEManager:
    def __init__(self, ui_q):
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

    def escanear(self):        self._go(self._scan())
    def conectar(self, addr):  self._go(self._connect(addr))
    def desconectar(self):     self._go(self._disconnect())
    def leer(self):            self._go(self._write(CHAR_JAULA, "READ"))
    def programar(self, j, d): self._go(self._programar(j, d))

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

    async def _write(self, uuid, value):
        if not self.connected:
            self._push("status", ("No conectado", RED))
            return
        try:
            await self.client.write_gatt_char(uuid, value.encode())
        except Exception as e:
            self._push("status", (f"Error escritura: {e}", RED))

    async def _programar(self, jaula, dolly):
        self._push("status", ("Programando...", YELLOW))
        await self._write(CHAR_DOLLY, str(dolly))
        await asyncio.sleep(0.3)
        await self._write(CHAR_JAULA, str(jaula))

    def _on_notify(self, sender, data):
        self._push("notify", data.decode("utf-8", errors="replace").strip())


# ─── Aplicación ───────────────────────────────────────────────────────────────
class App(tk.Tk):

    # ── Medidas de layout ─────────────────────────────────────────────────────
    HDR_H   = 65
    SB_H    = 55
    BODY_Y  = 65
    BODY_H  = H - 65 - 55    # 600px
    LEFT_W  = 488             # panel izquierdo
    SEP_X   = LEFT_W + 2      # 490
    RIGHT_X = LEFT_W + 4      # 492 — inicio panel derecho
    RIGHT_W = W - LEFT_W - 4  # 788px
    TAB_H   = 50              # alto de las pestañas
    FILT_W  = 308             # ancho del área de filtro/numpad
    LIST_X  = RIGHT_X + FILT_W + 4   # inicio de la lista

    def __init__(self):
        super().__init__()
        self.title("GIO - IDJ Programador")
        self.geometry(f"{W}x{H}+0+0")
        self.resizable(False, False)
        self.configure(bg=BG)
        self.attributes('-fullscreen', True)
        self.bind('<Escape>', lambda e: None)
        self.protocol("WM_DELETE_WINDOW", lambda: None)

        self.ui_q    = queue.Queue()
        self.ble     = BLEManager(self.ui_q)
        self.devices = []
        self.sel_dev = None

        # BLE state
        self.var_cur_jaula = tk.StringVar(value="—")
        self.var_cur_dolly = tk.StringVar(value="—")
        self.var_status    = tk.StringVar(value="Presiona ESCANEAR para buscar el IDJ Programador")
        self.var_ble_info  = tk.StringVar(value="Sin conexión")

        # Selección desde API
        self.selected_jaula   = None   # {"CODIGO", "num", "MODELO"}
        self.selected_dolly   = None
        self.tab              = "jaula"
        self.filtro           = ""
        self.jaulas_api       = []
        self.dollies_api      = []
        self.lista_filtrada   = []
        self.api_cargada      = False

        self.var_sel_jaula     = tk.StringVar(value="Sin seleccionar")
        self.var_sel_jaula_mod = tk.StringVar(value="")
        self.var_sel_dolly     = tk.StringVar(value="Sin dolly")
        self.var_sel_dolly_mod = tk.StringVar(value="Selecciona en la lista →")
        self.var_filtro        = tk.StringVar(value="")
        self.var_lista_titulo  = tk.StringVar(value="Jaulas disponibles")
        self.var_count         = tk.StringVar(value="")

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
        if name == "conn": self.scr_conn.tkraise()
        else:              self.scr_prog.tkraise()

    # =========================================================================
    # PANTALLA 1 — CONEXIÓN BLE
    # =========================================================================
    def _build_conn(self, p):
        hdr = tk.Frame(p, bg=PANEL, height=65)
        hdr.place(x=0, y=0, width=W, height=65)
        tk.Label(hdr, text="GIO  ·  IDJ PROGRAMADOR",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 22, "bold")).place(x=30, y=16)
        tk.Label(hdr, text="Conexión",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 16)).place(x=480, y=22)

        card = tk.Frame(p, bg=PANEL)
        card.place(x=240, y=85, width=800, height=575)

        tk.Label(card, text="Conectar al IDJ Programador",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 20, "bold")).place(x=0, y=20, width=800)
        tk.Label(card,
                 text="Asegúrate de que el IDJ Programador esté encendido y cerca",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 16)).place(x=0, y=58, width=800)

        self.btn_scan = self._btn(card, "⟳   ESCANEAR DISPOSITIVOS",
                                  BLUE, self._on_scan,
                                  x=200, y=108, w=400, h=72,
                                  font=("Arial", 17, "bold"))

        tk.Label(card, text="Dispositivos IDJ encontrados:",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 14, "bold")).place(x=50, y=202)

        lf = tk.Frame(card, bg=CARD)
        lf.place(x=50, y=230, width=700, height=120)
        self.lbox = tk.Listbox(lf, bg=CARD, fg=WHITE,
                               selectbackground=BLUE, selectforeground=WHITE,
                               font=("Arial", 16), height=4,
                               borderwidth=0, highlightthickness=0,
                               activestyle="none")
        self.lbox.pack(fill="both", expand=True, padx=6, pady=6)
        self.lbox.bind("<<ListboxSelect>>", self._on_select)

        self.btn_conn = self._btn(card, "CONECTAR  →",
                                  GREEN, self._on_connect,
                                  x=200, y=368, w=400, h=72,
                                  font=("Arial", 19, "bold"))

        # ── Configuración del dispositivo ──────────────────────────────────
        tk.Frame(card, bg=CARD, height=2).place(x=40, y=462, width=720, height=2)
        tk.Label(card, text="Configuración del dispositivo",
                 bg=PANEL, fg=GRAY, font=("Arial", 12)).place(x=50, y=473)

        self._btn(card, "⚙  Configurar WiFi",
                  TEAL, self._on_wifi,
                  x=200, y=498, w=195, h=50,
                  font=("Arial", 13, "bold"))
        self.btn_bt = self._btn(card, "⚡ Bluetooth ON",
                                GREEN, self._on_bt_toggle,
                                x=405, y=498, w=195, h=50,
                                font=("Arial", 13, "bold"))

        sb = tk.Frame(p, bg=PANEL, height=55)
        sb.place(x=0, y=H - 55, width=W, height=55)
        tk.Label(sb, text="Estado:", bg=PANEL, fg=GRAY,
                 font=("Arial", 13)).place(x=20, y=16)
        self.lbl_s1 = tk.Label(sb, textvariable=self.var_status,
                                bg=PANEL, fg=GREEN,
                                font=("Arial", 13, "bold"))
        self.lbl_s1.place(x=90, y=16)

    # =========================================================================
    # PANTALLA 2 — PROGRAMACIÓN
    # =========================================================================
    def _build_prog(self, p):
        # Header
        hdr = tk.Frame(p, bg=PANEL, height=self.HDR_H)
        hdr.place(x=0, y=0, width=W, height=self.HDR_H)
        self._btn(hdr, "←  VOLVER", CARD, self._on_volver,
                  x=15, y=12, w=140, h=42, font=("Arial", 13, "bold"))
        tk.Label(hdr, text="GIO  ·  IDJ PROGRAMADOR",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 18, "bold")).place(x=170, y=18)
        self.dot2 = tk.Label(hdr, text="●", bg=PANEL, fg=GREEN, font=("Arial", 14))
        self.dot2.place(x=W - 290, y=22)
        tk.Label(hdr, textvariable=self.var_ble_info,
                 bg=PANEL, fg=GRAY2, font=("Arial", 12)).place(x=W - 272, y=22)

        # Separador vertical entre paneles
        tk.Frame(p, bg=CARD).place(x=self.SEP_X, y=self.BODY_Y,
                                   width=2, height=self.BODY_H)

        self._build_left(p)
        self._build_right(p)

        # Status bar
        sb = tk.Frame(p, bg=PANEL, height=self.SB_H)
        sb.place(x=0, y=H - self.SB_H, width=W, height=self.SB_H)
        tk.Label(sb, text="Estado:", bg=PANEL, fg=GRAY,
                 font=("Arial", 13)).place(x=20, y=16)
        self.lbl_s2 = tk.Label(sb, textvariable=self.var_status,
                                bg=PANEL, fg=GREEN,
                                font=("Arial", 13, "bold"))
        self.lbl_s2.place(x=90, y=16)

    # ── Panel izquierdo ───────────────────────────────────────────────────────
    def _build_left(self, p):
        FW   = self.LEFT_W - 8    # ancho útil del formulario
        FX   = 8
        BY   = self.BODY_Y

        # Card: estado actual
        st = tk.Frame(p, bg=PANEL)
        st.place(x=FX, y=BY + 8, width=FW, height=205)

        tk.Label(st, text="ESCLAVO CONECTADO — ESTADO ACTUAL",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=10, y=8)

        cw = (FW - 28) // 2
        jc = tk.Frame(st, bg=CARD); jc.place(x=10, y=28, width=cw, height=115)
        tk.Label(jc, text="Jaula actual",
                 bg=CARD, fg=GRAY2, font=("Arial", 11)).place(x=8, y=8)
        tk.Label(jc, textvariable=self.var_cur_jaula,
                 bg=CARD, fg=GREEN, font=("Arial", 20, "bold"),
                 wraplength=cw-16).place(x=8, y=36)

        dc = tk.Frame(st, bg=CARD); dc.place(x=cw+18, y=28, width=cw, height=115)
        tk.Label(dc, text="Dolly actual",
                 bg=CARD, fg=GRAY2, font=("Arial", 11)).place(x=8, y=8)
        tk.Label(dc, textvariable=self.var_cur_dolly,
                 bg=CARD, fg=BLUE, font=("Arial", 20, "bold"),
                 wraplength=cw-16).place(x=8, y=36)

        self._btn(st, "↻   LEER EEPROM", BLUE, self._on_leer,
                  x=10, y=150, w=FW-20, h=46, font=("Arial", 13, "bold"))

        # Separador
        tk.Frame(p, bg=CARD).place(x=FX, y=BY+220, width=FW, height=2)

        # Card: A PROGRAMAR
        ap = tk.Frame(p, bg=PANEL)
        ap.place(x=FX, y=BY+228, width=FW, height=275)

        tk.Label(ap, text="A PROGRAMAR",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=10, y=8)

        # Jaula seleccionada
        jsel = tk.Frame(ap, bg=CARD); jsel.place(x=10, y=30, width=FW-20, height=98)
        tk.Label(jsel, text="Jaula",
                 bg=CARD, fg=GRAY2, font=("Arial", 11)).place(x=10, y=6)
        self.lbl_sel_jaula = tk.Label(jsel, textvariable=self.var_sel_jaula,
                                      bg=CARD, fg=GRAY2,
                                      font=("Arial", 22, "bold"),
                                      anchor="w")
        self.lbl_sel_jaula.place(x=10, y=28)
        tk.Label(jsel, textvariable=self.var_sel_jaula_mod,
                 bg=CARD, fg=GRAY, font=("Arial", 10)).place(x=10, y=68)

        # Dolly seleccionado
        dsel = tk.Frame(ap, bg=CARD); dsel.place(x=10, y=138, width=FW-20, height=98)
        tk.Label(dsel, text="Dolly",
                 bg=CARD, fg=GRAY2, font=("Arial", 11)).place(x=10, y=6)
        self.lbl_sel_dolly = tk.Label(dsel, textvariable=self.var_sel_dolly,
                                      bg=CARD, fg=GRAY2,
                                      font=("Arial", 22, "bold"),
                                      anchor="w")
        self.lbl_sel_dolly.place(x=10, y=28)
        tk.Label(dsel, textvariable=self.var_sel_dolly_mod,
                 bg=CARD, fg=GRAY, font=("Arial", 10)).place(x=10, y=68)

        # Botones PROGRAMAR / LIMPIAR
        BY2 = BY + 228 + 275 + 6
        BW_PROG = int((FW - 14) * 0.65)
        BW_LIMP = FW - 14 - BW_PROG - 10

        self.btn_prog = self._btn(p, "▶  PROGRAMAR",
                                  ORANGE, self._on_programar,
                                  x=FX+2, y=BY2, w=BW_PROG, h=62,
                                  font=("Arial", 15, "bold"))
        self._btn(p, "✕ LIMPIAR",
                  CARD, self._on_limpiar_sel,
                  x=FX+2+BW_PROG+10, y=BY2, w=BW_LIMP, h=62,
                  font=("Arial", 12, "bold"))

    # ── Panel derecho — selector con API ──────────────────────────────────────
    def _build_right(self, p):
        RX = self.RIGHT_X
        RY = self.BODY_Y
        RW = self.RIGHT_W
        TH = self.TAB_H
        FW = self.FILT_W
        LX = self.LIST_X
        LW = W - LX - 5

        # ── Pestañas ──────────────────────────────────────────────────────────
        tw = RW // 2
        self.btn_tab_j = self._btn(p, "⬜  JAULAS",
                                   BLUE, lambda: self._set_tab("jaula"),
                                   x=RX, y=RY, w=tw, h=TH,
                                   font=("Arial", 14, "bold"))
        self.btn_tab_d = self._btn(p, "⬜  DOLLIES",
                                   CARD, lambda: self._set_tab("dolly"),
                                   x=RX+tw, y=RY, w=tw, h=TH,
                                   font=("Arial", 14, "bold"))

        CY = RY + TH   # y inicio del contenido

        # ── Sub-separador vertical (filtro | lista) ───────────────────────────
        tk.Frame(p, bg=CARD).place(x=RX+FW+2, y=CY, width=2,
                                   height=self.BODY_H - TH)

        # ── Área de filtro + numpad ───────────────────────────────────────────
        tk.Label(p, text="FILTRAR POR NÚMERO:",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=RX+8, y=CY+10)

        # Display del filtro
        fd = tk.Frame(p, bg=BLUE, padx=2, pady=2)
        fd.place(x=RX+8, y=CY+32, width=FW-16, height=52)
        tk.Label(fd, textvariable=self.var_filtro,
                 bg=CARD, fg=WHITE,
                 font=("Arial", 28, "bold"),
                 anchor="e", padx=10).pack(fill="both", expand=True)

        # Numpad pequeño para filtrar
        NB_W, NB_H, NB_G = 86, 66, 6
        NB_TOTAL_W = 3*NB_W + 2*NB_G
        NB_OX = RX + (FW - NB_TOTAL_W)//2
        NB_OY = CY + 94

        keys = [
            ("7",0,0,CARD),("8",0,1,CARD),("9",0,2,CARD),
            ("4",1,0,CARD),("5",1,1,CARD),("6",1,2,CARD),
            ("1",2,0,CARD),("2",2,1,CARD),("3",2,2,CARD),
            ("←",3,0,YELLOW),("0",3,1,CARD),("✕",3,2,RED),
        ]
        for (txt, row, col, color) in keys:
            bx = NB_OX + col*(NB_W+NB_G)
            by = NB_OY + row*(NB_H+NB_G)
            if txt == "←":   cmd = self._on_filtro_back
            elif txt == "✕": cmd = self._on_filtro_clear
            else:             cmd = lambda t=txt: self._on_filtro_digit(t)
            tk.Button(p, text=txt, bg=color, fg=WHITE,
                      font=("Arial", 20, "bold"),
                      relief="flat",
                      activebackground=BLUE, activeforeground=WHITE,
                      command=cmd).place(x=bx, y=by, width=NB_W, height=NB_H)

        # Botón "SIN DOLLY" (solo visible en tab dollies)
        NB_BOTTOM_Y = NB_OY + 4*(NB_H+NB_G)
        self.btn_sin_dolly = tk.Button(
            p, text="○  SIN DOLLY",
            bg=PURPLE, fg=WHITE,
            font=("Arial", 13, "bold"),
            relief="flat",
            activebackground=WHITE, activeforeground=BG,
            command=self._on_sin_dolly)
        self.btn_sin_dolly.place(x=NB_OX, y=NB_BOTTOM_Y,
                                 width=NB_TOTAL_W, height=50)
        self.btn_sin_dolly.place_forget()   # oculto hasta que sea tab dolly

        # Botón actualizar API
        self._btn(p, "↻ Actualizar lista",
                  TEAL, self._cargar_api,
                  x=RX+8, y=CY+self.BODY_H-TH-52, w=FW-16, h=44,
                  font=("Arial", 11, "bold"))

        # ── Lista ─────────────────────────────────────────────────────────────
        tk.Label(p, textvariable=self.var_lista_titulo,
                 bg=BG, fg=GRAY2,
                 font=("Arial", 11, "bold")).place(x=LX+6, y=CY+10)
        tk.Label(p, textvariable=self.var_count,
                 bg=BG, fg=GRAY,
                 font=("Arial", 10)).place(x=LX+6, y=CY+30)

        # Scrollbar + listbox
        lf = tk.Frame(p, bg=CARD)
        lf.place(x=LX+4, y=CY+52, width=LW-4,
                 height=self.BODY_H - TH - 56)

        sb_list = tk.Scrollbar(lf, orient="vertical", bg=CARD,
                               troughcolor=CARD, activebackground=BLUE)
        sb_list.pack(side="right", fill="y")

        self.lbox_api = tk.Listbox(
            lf, bg=CARD, fg=WHITE,
            selectbackground=BLUE, selectforeground=WHITE,
            font=("Arial", 14), borderwidth=0,
            highlightthickness=0, activestyle="none",
            yscrollcommand=sb_list.set)
        self.lbox_api.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        sb_list.config(command=self.lbox_api.yview)
        self.lbox_api.bind("<<ListboxSelect>>", self._on_api_select)

        # Mensaje inicial
        self.lbox_api.insert("end", "  Cargando datos de la API...")

    # ─── Helper botón ─────────────────────────────────────────────────────────
    def _btn(self, parent, text, color, cmd, x, y, w, h, font=None):
        if font is None: font = ("Arial", 13, "bold")
        b = tk.Button(parent, text=text, bg=color, fg=WHITE, font=font,
                      relief="flat",
                      activebackground=WHITE, activeforeground=BG,
                      command=cmd)
        b.place(x=x, y=y, width=w, height=h)
        return b

    # ─── Pestañas ─────────────────────────────────────────────────────────────
    def _set_tab(self, tab):
        self.tab    = tab
        self.filtro = ""
        self.var_filtro.set("")

        if tab == "jaula":
            self.btn_tab_j.config(bg=BLUE)
            self.btn_tab_d.config(bg=CARD)
            self.var_lista_titulo.set("Jaulas disponibles")
            self.btn_sin_dolly.place_forget()
        else:
            self.btn_tab_d.config(bg=BLUE)
            self.btn_tab_j.config(bg=CARD)
            self.var_lista_titulo.set("Dollies disponibles")
            # Mostrar botón SIN DOLLY — recalcular posición
            NB_W, NB_H, NB_G = 86, 66, 6
            NB_TOTAL_W = 3*NB_W + 2*NB_G
            NB_OX = self.RIGHT_X + (self.FILT_W - NB_TOTAL_W)//2
            NB_BOTTOM_Y = (self.BODY_Y + self.TAB_H + 94) + 4*(NB_H+NB_G)
            self.btn_sin_dolly.place(x=NB_OX, y=NB_BOTTOM_Y,
                                     width=NB_TOTAL_W, height=50)

        self._actualizar_lista()

    # ─── API ──────────────────────────────────────────────────────────────────
    def _cargar_api(self):
        self._set_status("Consultando API...", YELLOW)
        self.lbox_api.delete(0, "end")
        self.lbox_api.insert("end", "  Consultando API, espera...")
        threading.Thread(target=self._fetch_thread, daemon=True).start()

    def _fetch_thread(self):
        try:
            j = self._fetch(API_JAULAS)
            d = self._fetch(API_DOLLIES)
            if j is not None: self.jaulas_api  = j
            if d is not None: self.dollies_api = d
            self.api_cargada = True
            self.ui_q.put({"tipo": "api_ok", "datos": None})
        except Exception as e:
            self.ui_q.put({"tipo": "api_error", "datos": str(e)})

    def _fetch(self, url):
        req = urllib.request.Request(url, headers={"User-Agent": "IDJ-App/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            return data["content"]["body"]

    def _actualizar_lista(self):
        lista = self.jaulas_api if self.tab == "jaula" else self.dollies_api
        f = self.filtro

        filtrada = []
        for item in lista:
            codigo = item.get("CODIGO", "")
            partes = codigo.split("-")
            num_part = partes[1] if len(partes) > 1 else codigo
            if not f or f in num_part:
                filtrada.append(item)

        self.lista_filtrada = filtrada
        self.lbox_api.delete(0, "end")

        if not lista:
            self.lbox_api.insert("end", "  Sin datos — presiona ↻ Actualizar lista")
            self.var_count.set("")
            return

        for item in filtrada:
            codigo = item.get("CODIGO", "")
            modelo = (item.get("MODELO") or "").strip()
            nome   = (item.get("NOME") or "").strip()[:28]
            self.lbox_api.insert(
                "end", f"  {codigo}   •   {modelo}   {nome}")

        total = len(lista)
        shown = len(filtrada)
        self.var_count.set(
            f"{shown} de {total}" if f else f"{total} equipos")

    def _on_api_select(self, _):
        sel = self.lbox_api.curselection()
        if not sel or not self.lista_filtrada: return
        idx = sel[0]
        if idx >= len(self.lista_filtrada): return

        item   = self.lista_filtrada[idx]
        codigo = item.get("CODIGO", "")
        partes = codigo.split("-")
        num_str = partes[1] if len(partes) > 1 else "0"
        num    = int(num_str)
        modelo = (item.get("MODELO") or "").strip()

        entry = {"CODIGO": codigo, "num": num, "MODELO": modelo}

        if self.tab == "jaula":
            self.selected_jaula = entry
            self.var_sel_jaula.set(codigo)
            self.var_sel_jaula_mod.set(modelo)
            self.lbl_sel_jaula.config(fg=GREEN)
            self._set_status(f"Jaula seleccionada: {codigo}", GREEN)
            # Auto-pasar a tab dolly
            self.after(300, lambda: self._set_tab("dolly"))
        else:
            self.selected_dolly = entry
            self.var_sel_dolly.set(codigo)
            self.var_sel_dolly_mod.set(modelo)
            self.lbl_sel_dolly.config(fg=BLUE)
            self._set_status(f"Dolly seleccionado: {codigo}", GREEN)

    # ─── Filtro numpad ────────────────────────────────────────────────────────
    def _on_filtro_digit(self, d):
        if len(self.filtro) < 4:
            self.filtro += d
            self.var_filtro.set(self.filtro)
            self._actualizar_lista()

    def _on_filtro_back(self):
        if self.filtro:
            self.filtro = self.filtro[:-1]
            self.var_filtro.set(self.filtro)
            self._actualizar_lista()

    def _on_filtro_clear(self):
        self.filtro = ""
        self.var_filtro.set("")
        self._actualizar_lista()

    def _on_sin_dolly(self):
        self.selected_dolly = None
        self.var_sel_dolly.set("Sin dolly")
        self.var_sel_dolly_mod.set("No se asignará dolly")
        self.lbl_sel_dolly.config(fg=GRAY2)
        self._set_status("Sin dolly — solo se programará la jaula", YELLOW)

    # ─── Callbacks BLE ────────────────────────────────────────────────────────
    def _on_scan(self):
        self.lbox.delete(0, "end")
        self.devices = []
        self.ble.escanear()

    def _on_select(self, _):
        sel = self.lbox.curselection()
        if sel: self.sel_dev = self.devices[sel[0]]

    def _on_connect(self):
        if self.sel_dev: self.ble.conectar(self.sel_dev.address)
        else:            self._set_status("Selecciona un dispositivo", YELLOW)

    def _on_volver(self):
        self.ble.desconectar()
        self._show("conn")

    def _on_leer(self):
        self.ble.leer()
        self._set_status("Leyendo EEPROM...", YELLOW)

    def _on_programar(self):
        if not self.selected_jaula:
            self._set_status("⚠  Selecciona una jaula de la lista →", YELLOW)
            return
        j = self.selected_jaula["num"]
        d = self.selected_dolly["num"] if self.selected_dolly else 0
        self.ble.programar(j, d)

    def _on_limpiar_sel(self):
        self.selected_jaula = None
        self.selected_dolly = None
        self.var_sel_jaula.set("Sin seleccionar")
        self.var_sel_jaula_mod.set("")
        self.var_sel_dolly.set("Sin dolly")
        self.var_sel_dolly_mod.set("Selecciona en la lista →")
        self.lbl_sel_jaula.config(fg=GRAY2)
        self.lbl_sel_dolly.config(fg=GRAY2)
        self._set_tab("jaula")

    # ─── Configuración WiFi / Bluetooth ───────────────────────────────────────
    def _on_wifi(self):
        try:
            subprocess.Popen(['lxterminal', '-e', 'nmtui'],
                             start_new_session=True)
        except Exception:
            try:
                subprocess.Popen(['x-terminal-emulator', '-e', 'nmtui'],
                                 start_new_session=True)
            except Exception as e:
                self._set_status(f"Abre una terminal y corre: nmtui", YELLOW)

    def _on_bt_toggle(self):
        try:
            result = subprocess.run(
                ['bluetoothctl', 'show'], capture_output=True, text=True)
            powered = 'Powered: yes' in result.stdout
            subprocess.run(['bluetoothctl', 'power',
                            'off' if powered else 'on'])
            if powered:
                self.btn_bt.config(bg=GRAY, text="⚡ Bluetooth OFF")
                self._set_status("Bluetooth apagado", GRAY)
            else:
                self.btn_bt.config(bg=GREEN, text="⚡ Bluetooth ON")
                self._set_status("Bluetooth encendido", GREEN)
        except Exception as e:
            self._set_status(f"Error BT: {e}", RED)

    # ─── Helpers ──────────────────────────────────────────────────────────────
    def _set_status(self, msg, color=GREEN):
        self.var_status.set(msg)
        self.lbl_s1.config(fg=color)
        self.lbl_s2.config(fg=color)

    # ─── Parser notificaciones BLE ────────────────────────────────────────────
    def _parse_notify(self, msg):
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
                            self.lbox.insert("end",
                                f"   {d.name}     ({d.address})")
                        self.lbox.selection_set(0)
                        self.sel_dev = dt[0]
                        self._set_status(
                            f"{len(dt)} dispositivo(s) IDJ encontrado(s) — presiona CONECTAR",
                            GREEN)
                    else:
                        self._set_status(
                            "No se encontraron dispositivos IDJ — intenta de nuevo",
                            YELLOW)

                elif tp == "connected":
                    self.dot2.config(fg=GREEN)
                    self.var_ble_info.set(
                        f"Conectado  •  "
                        f"{self.sel_dev.name if self.sel_dev else dt}")
                    self._set_status("Conectado — cargando datos de API y leyendo esclavo...",
                                     GREEN)
                    self.btn_prog.config(state="normal")
                    self._show("prog")
                    self._set_tab("jaula")
                    # Cargar API automáticamente al conectar
                    if not self.api_cargada:
                        self.after(500, self._cargar_api)

                elif tp == "disconnected":
                    self.dot2.config(fg=RED)
                    self.var_ble_info.set("Sin conexión")
                    self.var_cur_jaula.set("—")
                    self.var_cur_dolly.set("—")
                    self.btn_prog.config(state="disabled")
                    self._set_status("Desconectado", RED)
                    self._show("conn")

                elif tp == "notify":
                    self._parse_notify(dt)

                elif tp == "api_ok":
                    self._actualizar_lista()
                    total_j = len(self.jaulas_api)
                    total_d = len(self.dollies_api)
                    self._set_status(
                        f"API cargada: {total_j} jaulas, {total_d} dollies",
                        GREEN)

                elif tp == "api_error":
                    self.lbox_api.delete(0, "end")
                    self.lbox_api.insert("end",
                        "  ⚠  Error al consultar la API")
                    self.lbox_api.insert("end",
                        "  Verifica la conexión WiFi")
                    self.lbox_api.insert("end",
                        "  y presiona ↻ Actualizar lista")
                    self._set_status(
                        f"Error API: {dt} — verifica WiFi", RED)

                elif tp == "status":
                    self._set_status(*dt)

        except queue.Empty:
            pass

        self.after(100, self._poll)


if __name__ == "__main__":
    app = App()
    app.mainloop()
