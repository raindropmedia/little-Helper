#!/usr/bin/env python3
"""Parametrische Monitorhalterung für den M5Stack Dial v1.1 (Lenovo ThinkVision P27h-10).

Aufbau
  * Basis   – liegt auf der Monitor-Oberkante, Lippe greift vorne über den Rahmen,
              Manschette hält das Dial senkrecht (leicht nach unten geneigt).
  * Bein    – hängt hinten herunter und liegt am Monitorrücken an. Das Gelenk wird
              mit einer M4x30-Schraube + Mutter festgezogen -> passt für jede
              Gehäusedicke (wie bei Webcam-Halterungen).

Dial-Maße aus dem offiziellen Modell (M5_Hardware/Products/K130-V11_Dial_v1.1):
  Rückseite z=-17.3, Front z=+13.0, festes Gehäuse Ø49.7 (z -16.9..-4.3),
  Drehring Ø50.8 (z -0.9..+4.1) -> die Manschette endet bei z=-7 (6 mm Abstand
  zum Drehring). Anschlüsse liegen auf der Rückseite in einer Ø34-Öffnung.

Koordinaten: X = Breite, Y = Tiefe (+Y nach hinten, Monitor-Vorderkante Y=0),
Z = Höhe (Monitor-Oberkante Z=0).

Ausgabe:  python dial_halterung.py  -> STEP (Baugruppe + Einzelteile), STL
"""

import os

import cadquery as cq

# ------------------------------------------------------------------ Parameter
P = dict(
    # Dial
    dial_d=49.7,          # größter Durchmesser des festen Gehäuses im Manschettenbereich
    dial_back_z=-17.3,    # Rückseite (lokale Dial-Koordinate)
    dial_ring_d=50.8,     # Drehring
    dial_ring_z0=-0.9,    # Beginn Drehring
    dial_front_z=13.0,
    # Manschette
    fit_clear=0.30,       # radiales Spiel (vor den Klemmrippen)
    rib_h=0.45,           # Höhe der Klemmrippen (ergeben die Presspassung)
    rib_n=6,
    collar_wall=2.6,
    collar_front_z=-7.0,  # vordere Kante der Manschette (lokal), Abstand zum Drehring
    back_rim_t=2.4,       # Anlage hinten (nimmt den Druck beim Drücken auf)
    back_open_d=38.0,     # Öffnung für Kabel/Anschlüsse (Rückdeckel innen Ø34)
    tilt_deg=10.0,        # Display nach unten geneigt (zum Betrachter)
    dial_center_y=6.0,    # Lage der Dial-Mitte über der Platte (Y)
    lift=2.0,             # Abstand Drehring <-> Platte
    # Basis
    width=58.0,
    plate_t=3.5,
    plate_depth=38.0,     # muss >= Dicke der Monitor-Oberkante sein (Bein gleicht aus)
    lip_h=6.0,            # Lippe vorne (Rahmen des P27h-10 oben ca. 7 mm)
    lip_t=2.4,
    # Gelenk
    knuckle_r=5.5,
    hinge_w=44.0,         # Gesamtbreite der Gelenkaugen
    knuckle_gap=0.4,
    bolt_d=4.4,           # M4
    nut_af=7.3,           # M4-Mutter Schlüsselweite + Spiel
    nut_depth=3.4,
    head_d=8.6,           # M4 Zylinderkopf
    head_depth=3.0,
    # Bein
    leg_len=75.0,
    leg_t=4.5,
    leg_pad_w=26.0,
)


def collar_local(p):
    """Manschette + Sockel im Dial-Koordinatensystem (Achse = lokale z, oben = lokale y)."""
    r_in = p["dial_d"] / 2 + p["fit_clear"]
    r_out = r_in + p["collar_wall"]
    z0 = p["dial_back_z"] - p["back_rim_t"]
    z1 = p["collar_front_z"]
    length = z1 - z0

    collar = cq.Workplane("XY").workplane(offset=z0).circle(r_out).extrude(length)
    # Sockel nach unten (wird später an der Platte abgeschnitten)
    sock_w = 34.0
    sock = cq.Workplane("XY").workplane(offset=z0).center(0, -40).rect(sock_w, 80).extrude(length)
    body = collar.union(sock)

    # Bohrung für das Dial (vor der Rückanlage)
    bore = cq.Workplane("XY").workplane(offset=p["dial_back_z"]).circle(r_in).extrude(60)
    body = body.cut(bore)
    # Freiraum für Drehring / Front (großzügig)
    ring = (cq.Workplane("XY").workplane(offset=p["collar_front_z"])
            .circle(p["dial_ring_d"] / 2 + p["lift"]).extrude(40))
    body = body.cut(ring)
    # Rückanlage mit Öffnung (Anschlüsse, Kabel), 45°-Fase innen
    hole = (cq.Workplane("XY").workplane(offset=z0 - 1).circle(p["back_open_d"] / 2)
            .extrude(p["back_rim_t"] + 1.01))
    body = body.cut(hole)

    # Klemmrippen innen (Presspassung, beim Einschieben leicht gequetscht)
    rib_len = p["collar_front_z"] - p["dial_back_z"] - 1.0
    for i in range(p["rib_n"]):
        ang = 30 + i * 360 / p["rib_n"]
        rib = (cq.Workplane("XY").workplane(offset=p["dial_back_z"] + 0.5)
               .center(r_in - p["rib_h"] / 2 + 0.3, 0).rect(p["rib_h"] + 0.6, 1.6).extrude(rib_len)
               .rotate((0, 0, 0), (0, 0, 1), ang))
        body = body.union(rib)
    # Einführfase vorne an den Rippen: die Bohrung vorne 1 mm etwas weiter
    lead = (cq.Workplane("XY").workplane(offset=p["collar_front_z"] - 1.0)
            .circle(r_in).workplane(offset=1.0).circle(r_in + 0.4).loft())
    body = body.cut(lead)
    return body


def make_base(p):
    W, t, D = p["width"], p["plate_t"], p["plate_depth"]
    plate = cq.Workplane("XY").box(W, D, t, centered=(True, False, False))
    lip = (cq.Workplane("XY").box(W, p["lip_t"], p["lip_h"] + t, centered=(True, False, False))
           .translate((0, -p["lip_t"], -p["lip_h"])))
    base = plate.union(lip)
    base = base.edges("|X and <Z and <Y").fillet(1.2)

    # Dial-Mitte: Drehring muss 'lift' über der Platte frei laufen (Neigung berücksichtigt)
    zc = t + p["lift"] + p["dial_ring_d"] / 2 + 3.0
    cradle = (collar_local(p)
              .rotate((0, 0, 0), (1, 0, 0), 90 + p["tilt_deg"])
              .translate((0, p["dial_center_y"], zc)))
    # Sockel unterhalb der Plattenoberseite abschneiden
    keep = cq.Workplane("XY").box(200, 200, 200, centered=(True, True, False)).translate((0, 0, t - 0.01))
    cradle = cradle.intersect(keep)
    base = base.union(cradle)

    # Gelenkaugen hinten (außen), Achse entlang X
    yk, zk = D + p["knuckle_r"] - 1.0, t / 2
    seg = (p["hinge_w"] - (p["hinge_w"] / 2.2)) / 2  # Breite je Außenauge
    for sgn in (-1, 1):
        x_in = sgn * (p["hinge_w"] / 2 - seg)
        k = (cq.Workplane("YZ").workplane(offset=min(x_in, sgn * p["hinge_w"] / 2))
             .center(yk, zk).circle(p["knuckle_r"]).extrude(seg))
        web = (cq.Workplane("XY").box(seg, p["knuckle_r"] + 1, t, centered=False)
               .translate((min(x_in, sgn * p["hinge_w"] / 2), D - 1.5, 0)))
        base = base.union(k).union(web)
    # Bohrung, Kopfsenkung (links), Muttertasche (rechts)
    bolt = cq.Workplane("YZ").workplane(offset=-W).center(yk, zk).circle(p["bolt_d"] / 2).extrude(2 * W)
    head = (cq.Workplane("YZ").workplane(offset=-p["hinge_w"] / 2 - 1)
            .center(yk, zk).circle(p["head_d"] / 2).extrude(p["head_depth"] + 1))
    nut = (cq.Workplane("YZ").workplane(offset=p["hinge_w"] / 2 - p["nut_depth"])
           .center(yk, zk).polygon(6, p["nut_af"] / 0.8660254).extrude(p["nut_depth"] + 1))
    base = base.cut(bolt).cut(head).cut(nut)
    # Freiraum für das mittlere Gelenkauge des Beins (dreht sich frei)
    w_mid = p["hinge_w"] / 2.2 * 2
    free = (cq.Workplane("YZ").workplane(offset=-w_mid / 2).center(yk, zk)
            .circle(p["knuckle_r"] + 0.6).extrude(w_mid))
    base = base.cut(free)
    return base, (yk, zk), zc


def make_leg(p, hinge):
    yk, zk = hinge
    w_mid = p["hinge_w"] / 2.2 * 2 - 2 * p["knuckle_gap"]
    # Mittelauge
    k = (cq.Workplane("YZ").workplane(offset=-w_mid / 2).center(yk, zk)
         .circle(p["knuckle_r"]).extrude(w_mid))
    # Arm nach unten (gestreckt dargestellt; im Gebrauch zum Monitor geschwenkt)
    arm = (cq.Workplane("XY").box(w_mid, p["leg_t"], p["leg_len"], centered=(True, False, False))
           .translate((0, yk - p["leg_t"] / 2, zk - p["leg_len"])))
    leg = k.union(arm)
    # Fuß/Anlage am Monitorrücken, abgerundet
    pad = (cq.Workplane("XY").box(p["leg_pad_w"], p["leg_t"], 14, centered=(True, False, False))
           .translate((0, yk - p["leg_t"] / 2, zk - p["leg_len"])))
    leg = leg.union(pad).edges("|Y").fillet(2.0)
    # Kabelclip auf der Rückseite des Beins
    clip = (cq.Workplane("XZ").workplane(offset=-(yk + p["leg_t"] / 2))
            .center(0, zk - p["leg_len"] * 0.45)
            .rect(10, 8).extrude(-7))
    slot = (cq.Workplane("XZ").workplane(offset=-(yk + p["leg_t"] / 2 + 1.5))
            .center(0, zk - p["leg_len"] * 0.45).rect(12, 4.2).extrude(-4))
    leg = leg.union(clip).cut(slot)
    bolt = cq.Workplane("YZ").workplane(offset=-50).center(yk, zk).circle(p["bolt_d"] / 2).extrude(100)
    return leg.cut(bolt)


def build(p=P):
    base, hinge, zc = make_base(p)
    leg = make_leg(p, hinge)
    return base, leg, zc


if __name__ == "__main__":
    out = os.path.dirname(os.path.abspath(__file__))
    base, leg, zc = build()
    asm = cq.Assembly(name="Dial_Monitorhalterung")
    asm.add(base, name="Basis", color=cq.Color(0.85, 0.47, 0.34))
    asm.add(leg, name="Bein", color=cq.Color(0.25, 0.25, 0.25))
    asm.export(os.path.join(out, "dial_halterung_baugruppe.step"))
    cq.exporters.export(base, os.path.join(out, "dial_halterung_basis.step"))
    cq.exporters.export(leg, os.path.join(out, "dial_halterung_bein.step"))
    cq.exporters.export(base, os.path.join(out, "dial_halterung_basis.stl"), tolerance=0.05, angularTolerance=0.1)
    cq.exporters.export(leg, os.path.join(out, "dial_halterung_bein.stl"), tolerance=0.05, angularTolerance=0.1)
    # Passungstest: nur die Manschette (druckt ohne Stützen, ~15 min)
    r_out = P["dial_d"] / 2 + P["fit_clear"] + P["collar_wall"]
    ring = collar_local(P).intersect(cq.Workplane("XY").workplane(offset=-40).circle(r_out + 0.01).extrude(80))
    ring = ring.translate((0, 0, -(P["dial_back_z"] - P["back_rim_t"])))
    cq.exporters.export(ring, os.path.join(out, "passungstest_ring.stl"), tolerance=0.05, angularTolerance=0.1)
    print("Dial-Mitte Z =", round(zc, 2), "mm über Monitor-Oberkante")
    for n, s in (("Basis", base), ("Bein", leg)):
        bb = s.val().BoundingBox()
        print(f"{n}: {bb.xlen:.1f} x {bb.ylen:.1f} x {bb.zlen:.1f} mm, Volumen {s.val().Volume() / 1000:.1f} cm³")
