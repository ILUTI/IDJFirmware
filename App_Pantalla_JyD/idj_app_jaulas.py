#!/usr/bin/env python3
"""
GIO - IDJ Programador — Solo Jaulas
reTerminal 1280×720 — pantalla táctil
"""

import tkinter as tk
import asyncio
import threading
import queue
import re
import json
import os
import subprocess
import urllib.request
from datetime import datetime
from bleak import BleakScanner, BleakClient

# ─── API (solo jaulas) ────────────────────────────────────────────────────────
API_JAULAS = ("https://mcubackend.launion.com.gt/api/flujo/412/ejecutar/"
              "?public_key=YKlo4Q8yNNzVsgTGfJFUE5p0ZocgGmkL&responseIdentifier=1")

# ─── Cache ────────────────────────────────────────────────────────────────────
CACHE_DIR    = os.path.expanduser("~/.idj_cache_jaulas")
CACHE_JAULAS = os.path.join(CACHE_DIR, "jaulas.json")
CACHE_META   = os.path.join(CACHE_DIR, "meta.json")

# ─── BLE UUIDs ────────────────────────────────────────────────────────────────
CHAR_JAULA  = "0000a0b4-0000-1000-8000-00805f9b34fb"  # Write número / "READ"
CHAR_STATUS = "0000a0b5-0000-1000-8000-00805f9b34fb"  # Notify respuesta

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
    def programar(self, j):    self._go(self._programar(j))

    async def _scan(self):
        self._push("status", ("Escaneando BLE...", YELLOW))
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

    async def _programar(self, jaula):
        self._push("status", ("Programando...", YELLOW))
        await self._write(CHAR_JAULA, str(jaula))

    def _on_notify(self, sender, data):
        self._push("notify", data.decode("utf-8", errors="replace").strip())


# ─── Aplicación ───────────────────────────────────────────────────────────────
class App(tk.Tk):

    HDR_H   = 65
    SB_H    = 55
    BODY_Y  = 65
    BODY_H  = H - 65 - 55   # 600px
    LEFT_W  = 488
    SEP_X   = LEFT_W + 2
    RIGHT_X = LEFT_W + 4
    RIGHT_W = W - LEFT_W - 4

    def __init__(self):
        super().__init__()
        self.title("GIO - IDJ Programador — Solo Jaulas")
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

        # BLE / programación
        self.var_cur_jaula  = tk.StringVar(value="—")
        self.var_status     = tk.StringVar(value="Iniciando...")
        self.var_ble_info   = tk.StringVar(value="Sin conexión")
        self.selected_jaula = None   # {"CODIGO", "num", "NOME"}
        self.filtro         = ""
        self.jaulas_api     = []
        self.lista_filtrada = []
        self.api_cargada    = False
        self.var_sel_jaula     = tk.StringVar(value="Sin seleccionar")
        self.var_sel_jaula_mod = tk.StringVar(value="")
        self.var_filtro        = tk.StringVar(value="")
        self.var_count         = tk.StringVar(value="")
        self.var_ultima_act    = tk.StringVar(value="Sin datos guardados")

        # WiFi
        self.redes_wifi    = []
        self.sel_red       = None
        self.pwd_chars     = []
        self.pwd_visible   = False
        self.caps_on       = False
        self.wifi_encendido = True
        self.var_pwd_disp   = tk.StringVar(value="")
        self.var_red_sel    = tk.StringVar(value="Selecciona una red")
        self.var_wifi_msg   = tk.StringVar(value="")
        self.var_wifi_act   = tk.StringVar(value="")
        self.var_wifi_toggle = tk.StringVar(value="● WiFi  ON")

        ts = self._cargar_cache()
        if ts:
            self.var_ultima_act.set(f"Guardado: {ts}")
            self.var_status.set(f"Datos locales cargados ({ts})")
        else:
            self.var_status.set("Sin datos — conecta a internet y presiona Actualizar")

        self._build()
        self._show("conn")
        self._poll()
        self.after(3000, self._actualizar_api_silencioso)

    # ─── Navegación ───────────────────────────────────────────────────────────
    def _build(self):
        self.scr_conn       = tk.Frame(self, bg=BG)
        self.scr_prog       = tk.Frame(self, bg=BG)
        self.scr_wifi_redes = tk.Frame(self, bg=BG)
        self.scr_wifi_pwd   = tk.Frame(self, bg=BG)
        for s in (self.scr_conn, self.scr_prog,
                  self.scr_wifi_redes, self.scr_wifi_pwd):
            s.place(x=0, y=0, width=W, height=H)
        self._build_conn(self.scr_conn)
        self._build_prog(self.scr_prog)
        self._build_wifi_redes(self.scr_wifi_redes)
        self._build_wifi_pwd(self.scr_wifi_pwd)

    def _show(self, name):
        {"conn": self.scr_conn, "prog": self.scr_prog,
         "wifi_redes": self.scr_wifi_redes,
         "wifi_pwd":   self.scr_wifi_pwd}[name].tkraise()

    # =========================================================================
    # PANTALLA 1 — CONEXIÓN
    # =========================================================================
    def _build_conn(self, p):
        hdr = tk.Frame(p, bg=PANEL, height=65)
        hdr.place(x=0, y=0, width=W, height=65)
        tk.Label(hdr, text="GIO  ·  IDJ PROGRAMADOR — JAULAS",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 20, "bold")).place(x=30, y=18)

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
                 font=("Arial", 13)).place(x=20, y=8)
        self.lbl_s1 = tk.Label(sb, textvariable=self.var_status,
                                bg=PANEL, fg=GREEN,
                                font=("Arial", 12, "bold"))
        self.lbl_s1.place(x=90, y=8)
        self.lbl_ultima_act = tk.Label(sb, textvariable=self.var_ultima_act,
                                       bg=PANEL, fg=GRAY2,
                                       font=("Arial", 11))
        self.lbl_ultima_act.place(x=W - 320, y=10)
        self._btn(sb, "↻ Actualizar API",
                  TEAL, self._cargar_api,
                  x=W - 170, y=8, w=155, h=38,
                  font=("Arial", 11, "bold"))

    # =========================================================================
    # PANTALLA 2 — PROGRAMACIÓN (solo jaulas)
    # =========================================================================
    def _build_prog(self, p):
        hdr = tk.Frame(p, bg=PANEL, height=self.HDR_H)
        hdr.place(x=0, y=0, width=W, height=self.HDR_H)
        self._btn(hdr, "←  VOLVER", CARD, self._on_volver,
                  x=15, y=12, w=140, h=42, font=("Arial", 13, "bold"))
        tk.Label(hdr, text="GIO  ·  IDJ PROGRAMADOR — JAULAS",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 17, "bold")).place(x=170, y=18)
        self.dot2 = tk.Label(hdr, text="●", bg=PANEL, fg=GREEN, font=("Arial", 14))
        self.dot2.place(x=W - 290, y=22)
        tk.Label(hdr, textvariable=self.var_ble_info,
                 bg=PANEL, fg=GRAY2, font=("Arial", 12)).place(x=W - 272, y=22)

        tk.Frame(p, bg=CARD).place(x=self.SEP_X, y=self.BODY_Y,
                                   width=2, height=self.BODY_H)
        self._build_left(p)
        self._build_right(p)

        sb = tk.Frame(p, bg=PANEL, height=self.SB_H)
        sb.place(x=0, y=H - self.SB_H, width=W, height=self.SB_H)
        tk.Label(sb, text="Estado:", bg=PANEL, fg=GRAY,
                 font=("Arial", 13)).place(x=20, y=16)
        self.lbl_s2 = tk.Label(sb, textvariable=self.var_status,
                                bg=PANEL, fg=GREEN,
                                font=("Arial", 13, "bold"))
        self.lbl_s2.place(x=90, y=16)

    def _build_left(self, p):
        FW = self.LEFT_W - 8
        FX = 8
        BY = self.BODY_Y

        # Estado actual
        st = tk.Frame(p, bg=PANEL)
        st.place(x=FX, y=BY + 8, width=FW, height=180)
        tk.Label(st, text="ESCLAVO CONECTADO — JAULA ACTUAL",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=10, y=8)
        jc = tk.Frame(st, bg=CARD)
        jc.place(x=10, y=28, width=FW - 20, height=100)
        tk.Label(jc, text="Jaula programada",
                 bg=CARD, fg=GRAY2, font=("Arial", 12)).place(x=12, y=8)
        tk.Label(jc, textvariable=self.var_cur_jaula,
                 bg=CARD, fg=GREEN,
                 font=("Arial", 28, "bold")).place(x=12, y=38)
        self._btn(st, "↻   LEER EEPROM", BLUE, self._on_leer,
                  x=10, y=136, w=FW - 20, h=36,
                  font=("Arial", 13, "bold"))

        tk.Frame(p, bg=CARD).place(x=FX, y=BY + 196, width=FW, height=2)

        # A PROGRAMAR
        ap = tk.Frame(p, bg=PANEL)
        ap.place(x=FX, y=BY + 204, width=FW, height=280)
        tk.Label(ap, text="A PROGRAMAR",
                 bg=PANEL, fg=GRAY2,
                 font=("Arial", 10, "bold")).place(x=10, y=8)

        jsel = tk.Frame(ap, bg=CARD)
        jsel.place(x=10, y=30, width=FW - 20, height=120)
        tk.Label(jsel, text="Jaula seleccionada",
                 bg=CARD, fg=GRAY2, font=("Arial", 12)).place(x=10, y=8)
        self.lbl_sel_jaula = tk.Label(jsel, textvariable=self.var_sel_jaula,
                                      bg=CARD, fg=GRAY2,
                                      font=("Arial", 28, "bold"), anchor="w")
        self.lbl_sel_jaula.place(x=10, y=36)
        tk.Label(jsel, textvariable=self.var_sel_jaula_mod,
                 bg=CARD, fg=WHITE, font=("Arial", 13)).place(x=10, y=88)

        # Botones
        BY2 = BY + 204 + 280 + 8
        BW_PROG = int((FW - 14) * 0.65)
        BW_LIMP = FW - 14 - BW_PROG - 10
        self.btn_prog = self._btn(p, "▶  PROGRAMAR JAULA",
                                  ORANGE, self._on_programar,
                                  x=FX + 2, y=BY2, w=BW_PROG, h=70,
                                  font=("Arial", 15, "bold"))
        self._btn(p, "✕ LIMPIAR",
                  CARD, self._on_limpiar_sel,
                  x=FX + 2 + BW_PROG + 10, y=BY2, w=BW_LIMP, h=70,
                  font=("Arial", 12, "bold"))

    def _build_right(self, p):
        RX = self.RIGHT_X
        RY = self.BODY_Y
        RW = self.RIGHT_W
        LW = W - RX - 5

        # Título + filtro
        tk.Label(p, text="JAULAS DISPONIBLES",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 13, "bold")).place(x=RX + 8, y=RY + 8)
        tk.Label(p, textvariable=self.var_count,
                 bg=BG, fg=GRAY, font=("Arial", 11)).place(x=RX + 8, y=RY + 30)

        # Display filtro
        fd = tk.Frame(p, bg=BLUE, padx=2, pady=2)
        fd.place(x=RX + 8, y=RY + 52, width=RW // 2 - 16, height=52)
        tk.Label(fd, textvariable=self.var_filtro,
                 bg=CARD, fg=WHITE,
                 font=("Arial", 26, "bold"),
                 anchor="e", padx=10).pack(fill="both", expand=True)

        # Numpad filtro — ocupa la mitad izquierda del panel derecho
        NB_W, NB_H, NB_G = 116, 76, 8
        NB_TOTAL_W = 3 * NB_W + 2 * NB_G
        NB_OX = RX + (RW // 2 - NB_TOTAL_W) // 2
        NB_OY = RY + 114

        keys = [
            ("7",0,0,CARD),("8",0,1,CARD),("9",0,2,CARD),
            ("4",1,0,CARD),("5",1,1,CARD),("6",1,2,CARD),
            ("1",2,0,CARD),("2",2,1,CARD),("3",2,2,CARD),
            ("←",3,0,YELLOW),("0",3,1,CARD),("✕",3,2,RED),
        ]
        for (txt, row, col, color) in keys:
            bx = NB_OX + col * (NB_W + NB_G)
            by = NB_OY + row * (NB_H + NB_G)
            if txt == "←":   cmd = self._on_filtro_back
            elif txt == "✕": cmd = self._on_filtro_clear
            else:             cmd = lambda t=txt: self._on_filtro_digit(t)
            tk.Button(p, text=txt, bg=color, fg=WHITE,
                      font=("Arial", 22, "bold"), relief="flat",
                      activebackground=BLUE, activeforeground=WHITE,
                      command=cmd).place(x=bx, y=by, width=NB_W, height=NB_H)

        self._btn(p, "↻ Actualizar",
                  TEAL, self._cargar_api,
                  x=RX + 8, y=RY + self.BODY_H - 52, w=RW // 2 - 16, h=44,
                  font=("Arial", 11, "bold"))

        # Separador vertical interno
        tk.Frame(p, bg=CARD).place(x=RX + RW // 2 + 2, y=RY,
                                   width=2, height=self.BODY_H)

        # Lista de jaulas — mitad derecha
        LX = RX + RW // 2 + 6
        LW2 = W - LX - 5
        lf = tk.Frame(p, bg=CARD)
        lf.place(x=LX, y=RY + 4, width=LW2, height=self.BODY_H - 8)
        sb_list = tk.Scrollbar(lf, orient="vertical", bg=CARD,
                               troughcolor=CARD, activebackground=BLUE)
        sb_list.pack(side="right", fill="y")
        self.lbox_api = tk.Listbox(
            lf, bg=CARD, fg=WHITE,
            selectbackground=BLUE, selectforeground=WHITE,
            # ─── TAMAÑO DE LETRA — cambiar: 22=mediano  28=grande  34=muy grande
            font=("Arial", 28),
            borderwidth=0, highlightthickness=0, activestyle="none",
            yscrollcommand=sb_list.set)
        self.lbox_api.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        sb_list.config(command=self.lbox_api.yview)
        self.lbox_api.bind("<<ListboxSelect>>", self._on_api_select)
        self.lbox_api.insert("end", "  Cargando...")

    # ─── Helper botón ─────────────────────────────────────────────────────────
    def _btn(self, parent, text, color, cmd, x, y, w, h, font=None):
        if font is None: font = ("Arial", 13, "bold")
        b = tk.Button(parent, text=text, bg=color, fg=WHITE, font=font,
                      relief="flat",
                      activebackground=WHITE, activeforeground=BG,
                      command=cmd)
        b.place(x=x, y=y, width=w, height=h)
        return b

    # ─── Cache ────────────────────────────────────────────────────────────────
    def _cargar_cache(self):
        try:
            os.makedirs(CACHE_DIR, exist_ok=True)
            if os.path.exists(CACHE_JAULAS):
                with open(CACHE_JAULAS, 'r', encoding='utf-8') as f:
                    self.jaulas_api = json.load(f)
            if os.path.exists(CACHE_META):
                with open(CACHE_META, 'r') as f:
                    return json.load(f).get("ultima_actualizacion", "")
        except Exception:
            pass
        return ""

    def _guardar_cache(self, jaulas):
        try:
            os.makedirs(CACHE_DIR, exist_ok=True)
            with open(CACHE_JAULAS, 'w', encoding='utf-8') as f:
                json.dump(jaulas, f, ensure_ascii=False, indent=2)
            ts = datetime.now().strftime("%d/%m/%Y %H:%M")
            with open(CACHE_META, 'w') as f:
                json.dump({"ultima_actualizacion": ts}, f)
            return ts
        except Exception:
            return ""

    # ─── API ──────────────────────────────────────────────────────────────────
    def _cargar_api(self):
        self._set_status("Consultando API...", YELLOW)
        self.lbox_api.delete(0, "end")
        self.lbox_api.insert("end", "  Consultando API...")
        threading.Thread(target=self._fetch_thread, daemon=True).start()

    def _actualizar_api_silencioso(self):
        threading.Thread(target=self._fetch_silencioso, daemon=True).start()

    def _fetch_thread(self):
        try:
            j = self._fetch(API_JAULAS)
            if j:
                self.jaulas_api = j
                ts = self._guardar_cache(j)
                self.api_cargada = True
                self.ui_q.put({"tipo": "api_ok", "datos": ts})
            else:
                self.ui_q.put({"tipo": "api_error", "datos": "Respuesta vacía"})
        except Exception as e:
            self.ui_q.put({"tipo": "api_error", "datos": str(e)})

    def _fetch_silencioso(self):
        try:
            j = self._fetch(API_JAULAS)
            if j:
                self.jaulas_api = j
                ts = self._guardar_cache(j)
                self.api_cargada = True
                self.ui_q.put({"tipo": "api_ok_silencioso", "datos": ts})
        except Exception:
            pass

    def _fetch(self, url):
        req = urllib.request.Request(url, headers={"User-Agent": "IDJ-App/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            return data["content"]["body"]

    def _actualizar_lista(self):
        f = self.filtro
        filtrada = []
        for item in self.jaulas_api:
            codigo = item.get("CODIGO", "")
            partes = codigo.split("-")
            num_part = partes[1] if len(partes) > 1 else codigo
            if not f or f in num_part:
                filtrada.append(item)
        self.lista_filtrada = filtrada
        self.lbox_api.delete(0, "end")
        if not self.jaulas_api:
            self.lbox_api.insert("end", "  Sin datos — presiona ↻ Actualizar")
            self.var_count.set("")
            return
        for item in filtrada:
            self.lbox_api.insert("end", f"   {item.get('CODIGO', '')}")
        self.var_count.set(
            f"{len(filtrada)} de {len(self.jaulas_api)}" if f
            else f"{len(self.jaulas_api)} jaulas")

    def _on_api_select(self, _):
        sel = self.lbox_api.curselection()
        if not sel or not self.lista_filtrada: return
        idx = sel[0]
        if idx >= len(self.lista_filtrada): return
        item   = self.lista_filtrada[idx]
        codigo = item.get("CODIGO", "")
        partes = codigo.split("-")
        num    = int(partes[1]) if len(partes) > 1 else 0
        nome   = (item.get("NOME") or "").strip()
        self.selected_jaula = {"CODIGO": codigo, "num": num, "NOME": nome}
        self.var_sel_jaula.set(codigo)
        self.var_sel_jaula_mod.set(nome)
        self.lbl_sel_jaula.config(fg=GREEN)
        self._set_status(f"Jaula seleccionada: {codigo}", GREEN)

    # ─── Filtro ───────────────────────────────────────────────────────────────
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

    # ─── BLE callbacks ────────────────────────────────────────────────────────
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
        self.ble.programar(self.selected_jaula["num"])

    def _on_limpiar_sel(self):
        self.selected_jaula = None
        self.var_sel_jaula.set("Sin seleccionar")
        self.var_sel_jaula_mod.set("")
        self.lbl_sel_jaula.config(fg=GRAY2)

    def _on_wifi(self):
        red = self._get_wifi_actual()
        self.var_wifi_act.set(f"Conectado a: {red}" if red else "Sin conexión WiFi")
        self._show("wifi_redes")
        self.after(300, self._on_escanear_wifi)

    def _on_bt_toggle(self):
        try:
            result = subprocess.run(['bluetoothctl', 'show'],
                                   capture_output=True, text=True)
            powered = 'Powered: yes' in result.stdout
            subprocess.run(['bluetoothctl', 'power', 'off' if powered else 'on'])
            if powered:
                self.btn_bt.config(bg=GRAY, text="⚡ Bluetooth OFF")
                self._set_status("Bluetooth apagado", GRAY)
            else:
                self.btn_bt.config(bg=GREEN, text="⚡ Bluetooth ON")
                self._set_status("Bluetooth encendido", GREEN)
        except Exception as e:
            self._set_status(f"Error BT: {e}", RED)

    # ─── WiFi ─────────────────────────────────────────────────────────────────
    def _build_wifi_redes(self, p):
        hdr = tk.Frame(p, bg=PANEL, height=65)
        hdr.place(x=0, y=0, width=W, height=65)
        self._btn(hdr, "←  VOLVER", CARD, lambda: self._show("conn"),
                  x=15, y=12, w=150, h=42, font=("Arial", 13, "bold"))
        tk.Label(hdr, text="GIO  ·  CONFIGURACIÓN WIFI",
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 19, "bold")).place(x=180, y=18)
        tk.Label(hdr, textvariable=self.var_wifi_act,
                 bg=PANEL, fg=GREEN, font=("Arial", 12)).place(x=W - 310, y=22)

        cy = 75
        self.btn_wifi_toggle = tk.Button(
            p, textvariable=self.var_wifi_toggle, bg=GREEN, fg=WHITE,
            font=("Arial", 15, "bold"), relief="flat",
            activebackground=WHITE, activeforeground=BG,
            command=self._on_wifi_toggle)
        self.btn_wifi_toggle.place(x=15, y=cy, width=240, height=60)
        self._btn(p, "⟳  ESCANEAR REDES", BLUE, self._on_escanear_wifi,
                  x=268, y=cy, w=280, h=60, font=("Arial", 15, "bold"))
        self.btn_sel_red = tk.Button(
            p, text="SELECCIONAR ESTA RED  →", bg=GRAY, fg=WHITE,
            font=("Arial", 17, "bold"), relief="flat",
            activebackground=WHITE, activeforeground=BG,
            state="disabled", command=self._on_ir_a_pwd)
        self.btn_sel_red.place(x=W - 450, y=cy, width=435, height=60)

        tk.Label(p, text="REDES DISPONIBLES",
                 bg=BG, fg=GRAY2,
                 font=("Arial", 13, "bold")).place(x=15, y=cy + 72)
        lf = tk.Frame(p, bg=CARD)
        lf.place(x=15, y=cy + 100, width=W - 30, height=430)
        sb_w = tk.Scrollbar(lf, orient="vertical", bg=CARD,
                            troughcolor=CARD, activebackground=BLUE)
        sb_w.pack(side="right", fill="y")
        self.lbox_wifi = tk.Listbox(
            lf, bg=CARD, fg=WHITE, selectbackground=BLUE,
            selectforeground=WHITE, font=("Arial", 26),
            borderwidth=0, highlightthickness=0, activestyle="none",
            yscrollcommand=sb_w.set)
        self.lbox_wifi.pack(side="left", fill="both", expand=True, padx=8, pady=8)
        sb_w.config(command=self.lbox_wifi.yview)
        self.lbox_wifi.bind("<<ListboxSelect>>", self._on_red_select)

        sb2 = tk.Frame(p, bg=PANEL, height=50)
        sb2.place(x=0, y=H - 50, width=W, height=50)
        self.lbl_wifi_msg = tk.Label(sb2, textvariable=self.var_wifi_msg,
                                     bg=PANEL, fg=GREEN,
                                     font=("Arial", 13, "bold"))
        self.lbl_wifi_msg.place(x=20, y=12)
        self.after(200, self._verificar_wifi_estado)

    def _build_wifi_pwd(self, p):
        hdr = tk.Frame(p, bg=PANEL, height=65)
        hdr.place(x=0, y=0, width=W, height=65)
        self._btn(hdr, "←  REDES", CARD, lambda: self._show("wifi_redes"),
                  x=15, y=12, w=150, h=42, font=("Arial", 13, "bold"))
        self.lbl_pwd_hdr = tk.Label(hdr, text="Ingresar contraseña",
                                    bg=PANEL, fg=WHITE,
                                    font=("Arial", 17, "bold"))
        self.lbl_pwd_hdr.place(x=180, y=18)

        info = tk.Frame(p, bg=PANEL); info.place(x=0, y=65, width=W, height=58)
        tk.Label(info, textvariable=self.var_red_sel,
                 bg=PANEL, fg=WHITE,
                 font=("Arial", 18, "bold")).place(x=20, y=12)

        tk.Label(p, text="Contraseña:", bg=BG, fg=GRAY2,
                 font=("Arial", 13)).place(x=20, y=132)
        pf = tk.Frame(p, bg=BLUE, padx=3, pady=3)
        pf.place(x=20, y=155, width=W - 100, height=65)
        self.lbl_pwd = tk.Label(pf, textvariable=self.var_pwd_disp,
                                bg=CARD, fg=WHITE,
                                font=("Arial", 28, "bold"),
                                anchor="w", padx=14)
        self.lbl_pwd.pack(fill="both", expand=True)
        self.btn_eye = self._btn(p, "👁", CARD, self._on_toggle_pwd,
                                 x=W - 72, y=155, w=62, h=65,
                                 font=("Arial", 18))
        self._build_teclado_grande(p, y=232)

        self._btn(p, "✓  CONECTAR A RED", GREEN, self._on_conectar_wifi,
                  x=20, y=H - 108, w=580, h=65,
                  font=("Arial", 18, "bold"))
        self._btn(p, "✕  BORRAR CONTRASEÑA", CARD, self._on_borrar_pwd,
                  x=618, y=H - 108, w=310, h=65,
                  font=("Arial", 14, "bold"))
        sb = tk.Frame(p, bg=PANEL, height=43)
        sb.place(x=0, y=H - 43, width=W, height=43)
        self.lbl_pwd_msg = tk.Label(sb, textvariable=self.var_wifi_msg,
                                    bg=PANEL, fg=GREEN,
                                    font=("Arial", 13, "bold"))
        self.lbl_pwd_msg.place(x=20, y=10)

    def _build_teclado_grande(self, p, y):
        MARGIN = 15; AVAIL = W - 2 * MARGIN; KH = 55; KG = 6
        filas = [
            ['1','2','3','4','5','6','7','8','9','0','-','_'],
            ['q','w','e','r','t','y','u','i','o','p'],
            ['a','s','d','f','g','h','j','k','l'],
            ['z','x','c','v','b','n','m','.','@','#'],
        ]
        for r, fila in enumerate(filas):
            n = len(fila); kw = (AVAIL - (n-1)*KG) // n
            ox = MARGIN + (AVAIL - (n*kw + (n-1)*KG)) // 2
            for c, ch in enumerate(fila):
                bx = ox + c*(kw+KG); by = y + r*(KH+KG)
                tk.Button(p, text=ch.upper() if self.caps_on else ch,
                          bg=CARD, fg=WHITE, font=("Arial", 17, "bold"),
                          relief="flat", activebackground=BLUE,
                          activeforeground=WHITE,
                          command=lambda k=ch: self._on_tecla(k)
                          ).place(x=bx, y=by, width=kw, height=KH)
        ey = y + 4*(KH+KG)
        w_caps=170; w_excl=85; w_pct=85; w_back=120
        w_sp = AVAIL - w_caps - w_excl - w_pct - w_back - 4*KG
        esp = [("⇧ CAPS",w_caps,TEAL,self._on_caps),
               ("ESPACIO",w_sp,CARD,lambda: self._on_tecla(' ')),
               ("!",w_excl,CARD,lambda: self._on_tecla('!')),
               ("%",w_pct,CARD,lambda: self._on_tecla('%')),
               ("←",w_back,YELLOW,self._on_backspace_wifi)]
        ex = MARGIN
        for (txt,w,color,cmd) in esp:
            tk.Button(p, text=txt, bg=color, fg=WHITE,
                      font=("Arial", 14, "bold"), relief="flat",
                      activebackground=BLUE, activeforeground=WHITE,
                      command=cmd).place(x=ex, y=ey, width=w, height=KH)
            ex += w + KG
        self._teclado_parent=p; self._teclado_y=y
        self._teclado_kh=KH; self._teclado_kg=KG
        self._teclado_filas=filas; self._teclado_avail=AVAIL
        self._teclado_margin=MARGIN

    def _actualizar_teclado_caps(self):
        AVAIL=self._teclado_avail; MARGIN=self._teclado_margin
        KH=self._teclado_kh; KG=self._teclado_kg
        p=self._teclado_parent; y=self._teclado_y
        for r, fila in enumerate(self._teclado_filas):
            n=len(fila); kw=(AVAIL-(n-1)*KG)//n
            ox=MARGIN+(AVAIL-(n*kw+(n-1)*KG))//2
            for c, ch in enumerate(fila):
                bx=ox+c*(kw+KG); by=y+r*(KH+KG)
                for w in p.place_slaves():
                    info=w.place_info()
                    if int(info.get('x',-1))==bx and int(info.get('y',-1))==by:
                        w.config(text=ch.upper() if self.caps_on else ch); break

    def _verificar_wifi_estado(self):
        try:
            r = subprocess.run(['nmcli','radio','wifi'],
                               capture_output=True, text=True, timeout=5)
            self.wifi_encendido = 'enabled' in r.stdout.lower()
        except: self.wifi_encendido = True
        self._actualizar_btn_wifi()

    def _actualizar_btn_wifi(self):
        if self.wifi_encendido:
            self.var_wifi_toggle.set("● WiFi  ON")
            self.btn_wifi_toggle.config(bg=GREEN)
        else:
            self.var_wifi_toggle.set("○ WiFi  OFF")
            self.btn_wifi_toggle.config(bg=GRAY)

    def _on_wifi_toggle(self):
        try:
            nuevo = 'off' if self.wifi_encendido else 'on'
            subprocess.run(['nmcli','radio','wifi',nuevo], timeout=8)
            self.wifi_encendido = not self.wifi_encendido
            self._actualizar_btn_wifi()
            if self.wifi_encendido:
                self.var_wifi_msg.set("WiFi encendido — escaneando...")
                self.lbl_wifi_msg.config(fg=GREEN)
                self.after(1200, self._on_escanear_wifi)
            else:
                self.var_wifi_msg.set("WiFi apagado")
                self.lbl_wifi_msg.config(fg=GRAY)
                self.lbox_wifi.delete(0, "end")
        except Exception as e:
            self.var_wifi_msg.set(f"Error: {e}")
            self.lbl_wifi_msg.config(fg=RED)

    def _on_escanear_wifi(self):
        self.lbox_wifi.delete(0, "end")
        self.lbox_wifi.insert("end", "  Escaneando...")
        self.var_wifi_msg.set("Escaneando redes WiFi...")
        self.lbl_wifi_msg.config(fg=YELLOW)
        threading.Thread(target=self._escanear_thread, daemon=True).start()

    def _escanear_thread(self):
        try:
            r = subprocess.run(
                ['nmcli','--terse','--fields','SSID,SIGNAL,SECURITY',
                 'device','wifi','list','--rescan','yes'],
                capture_output=True, text=True, timeout=15)
            redes = []; vistas = set()
            for line in r.stdout.strip().split('\n'):
                if not line.strip(): continue
                partes = line.rsplit(':',2)
                if len(partes) < 2: continue
                ssid = partes[0].replace('\\:',':').strip()
                signal = partes[1].strip() if len(partes)>1 else "0"
                security = partes[2].strip() if len(partes)>2 else ""
                if not ssid or ssid=='--' or ssid in vistas: continue
                vistas.add(ssid)
                redes.append({"ssid":ssid,"signal":signal,"segura":bool(security)})
            redes.sort(key=lambda r: int(r["signal"]) if r["signal"].isdigit() else 0,
                       reverse=True)
            self.ui_q.put({"tipo":"wifi_scan_ok","datos":redes})
        except Exception as e:
            self.ui_q.put({"tipo":"wifi_scan_error","datos":str(e)})

    def _on_red_select(self, _):
        sel = self.lbox_wifi.curselection()
        if not sel or not self.redes_wifi: return
        idx = sel[0]
        if idx >= len(self.redes_wifi): return
        self.sel_red = self.redes_wifi[idx]
        self.pwd_chars = []; self.var_pwd_disp.set("")
        self.btn_sel_red.config(state="normal", bg=GREEN)
        if not self.sel_red["segura"]:
            self.var_wifi_msg.set("Red abierta — presiona CONECTAR directamente")
            self.lbl_wifi_msg.config(fg=YELLOW)

    def _on_ir_a_pwd(self):
        if not self.sel_red: return
        ssid = self.sel_red["ssid"]; segura = self.sel_red["segura"]
        barras = self._barras_senal(self.sel_red.get("signal","0"))
        candado = "🔒 Red segura" if segura else "🔓 Red abierta"
        self.lbl_pwd_hdr.config(text=f"Contraseña para:  {ssid}")
        self.var_red_sel.set(f"  {barras}   {ssid}   —   {candado}")
        self.pwd_chars=[]; self.pwd_visible=False; self.var_pwd_disp.set("")
        msg = f"Ingresa la contraseña de  {ssid}" if segura \
              else "Red abierta — presiona CONECTAR directamente"
        self.var_wifi_msg.set(msg)
        self.lbl_wifi_msg.config(fg=GRAY2 if segura else YELLOW)
        self._show("wifi_pwd")

    def _on_tecla(self, ch):
        if self.caps_on and ch.isalpha(): ch = ch.upper()
        self.pwd_chars.append(ch); self._refrescar_pwd()

    def _on_caps(self):
        self.caps_on = not self.caps_on; self._actualizar_teclado_caps()

    def _on_backspace_wifi(self):
        if self.pwd_chars: self.pwd_chars.pop(); self._refrescar_pwd()

    def _on_borrar_pwd(self):
        self.pwd_chars = []; self._refrescar_pwd()

    def _on_toggle_pwd(self):
        self.pwd_visible = not self.pwd_visible; self._refrescar_pwd()
        self.btn_eye.config(fg=GREEN if self.pwd_visible else WHITE)

    def _refrescar_pwd(self):
        self.var_pwd_disp.set(
            ''.join(self.pwd_chars) if self.pwd_visible
            else '●' * len(self.pwd_chars))

    def _on_conectar_wifi(self):
        if not self.sel_red:
            self.var_wifi_msg.set("⚠  Selecciona una red primero")
            self.lbl_wifi_msg.config(fg=YELLOW); return
        ssid = self.sel_red["ssid"]; pwd = ''.join(self.pwd_chars)
        self.var_wifi_msg.set(f"Conectando a {ssid}...")
        self.lbl_wifi_msg.config(fg=YELLOW)
        threading.Thread(target=self._conectar_thread,
                         args=(ssid, pwd), daemon=True).start()

    def _conectar_thread(self, ssid, pwd):
        try:
            cmd = ['nmcli','dev','wifi','connect', ssid]
            if pwd: cmd += ['password', pwd]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            exito = ('successfully activated' in r.stdout.lower() or
                     'already connected' in r.stdout.lower())
            self.ui_q.put({"tipo":"wifi_connect_ok" if exito else "wifi_connect_fail",
                           "datos":ssid})
        except Exception as e:
            self.ui_q.put({"tipo":"wifi_connect_fail","datos":str(e)})

    def _get_wifi_actual(self):
        try:
            r = subprocess.run(['nmcli','-t','-f','DEVICE,STATE,CONNECTION',
                                'device','status'],
                               capture_output=True, text=True, timeout=5)
            for line in r.stdout.strip().split('\n'):
                if 'wifi' in line.lower() and ':connected:' in line.lower():
                    return line.split(':')[2]
        except: pass
        return None

    @staticmethod
    def _barras_senal(s):
        try: s=int(s)
        except: return "░░░░"
        if s>=80: return "████"
        if s>=60: return "███░"
        if s>=40: return "██░░"
        if s>=20: return "█░░░"
        return "░░░░"

    # ─── Helpers ──────────────────────────────────────────────────────────────
    def _set_status(self, msg, color=GREEN):
        self.var_status.set(msg)
        self.lbl_s1.config(fg=color)
        self.lbl_s2.config(fg=color)

    # ─── Parser BLE ───────────────────────────────────────────────────────────
    def _parse_notify(self, msg):
        if msg.startswith("READ:"):
            if "VIRGEN" in msg:
                self.var_cur_jaula.set("Sin programar")
                self._set_status("Esclavo sin programar aún", YELLOW)
            else:
                m = re.match(r"READ:jaula=(\d+)", msg)
                if m:
                    self.var_cur_jaula.set(f"T0603-{int(m.group(1)):04d}")
                    self._set_status("EEPROM leída correctamente", GREEN)
            return
        if msg.startswith("READ_ERROR:"):
            self.var_cur_jaula.set("Error")
            self._set_status(f"⚠  {msg.split(':',1)[1].replace('_',' ')}", RED)
            return
        if msg.startswith("OK!"):
            self._set_status(f"✅  {msg}", GREEN)
            self.after(900, self._on_leer)
            return
        if msg.startswith("ERROR:"):
            self._set_status(f"❌  {msg.split(':',1)[1]}", RED)
            return
        self._set_status(msg, GRAY2)

    # ─── Queue polling ────────────────────────────────────────────────────────
    def _poll(self):
        try:
            while True:
                m  = self.ui_q.get_nowait()
                tp = m["tipo"]; dt = m["datos"]

                if tp == "scan_ok":
                    self.devices = dt; self.lbox.delete(0, "end")
                    if dt:
                        for d in dt:
                            self.lbox.insert("end", f"   {d.name}  ({d.address})")
                        self.lbox.selection_set(0); self.sel_dev = dt[0]
                        self._set_status(
                            f"{len(dt)} dispositivo(s) — presiona CONECTAR", GREEN)
                    else:
                        self._set_status("No se encontraron dispositivos IDJ", YELLOW)

                elif tp == "connected":
                    self.dot2.config(fg=GREEN)
                    self.var_ble_info.set(
                        f"Conectado  •  {self.sel_dev.name if self.sel_dev else dt}")
                    self._set_status("Conectado — leyendo jaula actual...", GREEN)
                    self.btn_prog.config(state="normal")
                    self._show("prog")
                    if not self.api_cargada:
                        self.after(500, self._cargar_api)

                elif tp == "disconnected":
                    self.dot2.config(fg=RED)
                    self.var_ble_info.set("Sin conexión")
                    self.var_cur_jaula.set("—")
                    self.btn_prog.config(state="disabled")
                    self._set_status("Desconectado", RED)
                    self._show("conn")

                elif tp == "notify":
                    self._parse_notify(dt)

                elif tp in ("api_ok", "api_ok_silencioso"):
                    self._actualizar_lista()
                    ts = dt or ""
                    self.var_ultima_act.set(f"Actualizado: {ts}")
                    self.lbl_ultima_act.config(fg=GREEN)
                    if tp == "api_ok":
                        self._set_status(
                            f"✅ API guardada: {len(self.jaulas_api)} jaulas ({ts})",
                            GREEN)

                elif tp == "api_error":
                    if self.jaulas_api:
                        self._actualizar_lista()
                        self._set_status(
                            f"Sin internet — datos guardados ({self.var_ultima_act.get()})",
                            YELLOW)
                    else:
                        self.lbox_api.delete(0,"end")
                        self.lbox_api.insert("end","  ⚠  Sin datos — verifica WiFi")
                        self._set_status(f"Error API: {dt}", RED)

                elif tp == "wifi_scan_ok":
                    self.redes_wifi = dt; self.lbox_wifi.delete(0,"end")
                    if dt:
                        for red in dt:
                            b = self._barras_senal(red["signal"])
                            c = " 🔒" if red["segura"] else ""
                            self.lbox_wifi.insert("end", f"  {b}  {red['ssid']}{c}")
                        self.var_wifi_msg.set(f"{len(dt)} redes — toca una para seleccionar")
                        self.lbl_wifi_msg.config(fg=GREEN)
                    else:
                        self.lbox_wifi.insert("end","  No se encontraron redes")
                        self.lbl_wifi_msg.config(fg=YELLOW)

                elif tp == "wifi_scan_error":
                    self.lbox_wifi.delete(0,"end")
                    self.lbox_wifi.insert("end", f"  Error: {dt}")
                    self.lbl_wifi_msg.config(fg=RED)

                elif tp == "wifi_connect_ok":
                    self.var_wifi_msg.set(f"✅  Conectado a {dt}")
                    self.lbl_wifi_msg.config(fg=GREEN)
                    self.var_wifi_act.set(f"Conectado a: {dt}")
                    self.pwd_chars=[]; self.var_pwd_disp.set("")
                    self.after(1000, self._actualizar_api_silencioso)

                elif tp == "wifi_connect_fail":
                    self.var_wifi_msg.set(f"❌  Error: {dt}")
                    self.lbl_wifi_msg.config(fg=RED)

                elif tp == "status":
                    self._set_status(*dt)

        except queue.Empty:
            pass
        self.after(100, self._poll)


if __name__ == "__main__":
    app = App()
    app.mainloop()
