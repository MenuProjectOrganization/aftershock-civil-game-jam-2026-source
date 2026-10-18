"""Generate the small, static low-poly citizen mesh source used by the ISM crowd.

The proxy is intentionally an authored static mesh: the live crowd stays one
InstancedStaticMeshComponent (no per-citizen actors or skeletal animation).
Run with the repository Python interpreter; the resulting OBJ is imported by UE 5.8.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "Content" / "Aftershock" / "KenneyMeshes" / "_Imports" / "SM_CitizenProxy.obj"


class ObjBuilder:
    def __init__(self):
        self.vertices = []
        self.faces = []

    def box(self, cx, cy, cz, sx, sy, sz):
        x0, x1 = cx - sx / 2, cx + sx / 2
        y0, y1 = cy - sy / 2, cy + sy / 2
        z0, z1 = cz - sz / 2, cz + sz / 2
        start = len(self.vertices) + 1
        self.vertices += [
            (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
            (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
        ]
        self.faces += [
            (start, start + 3, start + 2, start + 1),
            (start + 4, start + 5, start + 6, start + 7),
            (start, start + 1, start + 5, start + 4),
            (start + 1, start + 2, start + 6, start + 5),
            (start + 2, start + 3, start + 7, start + 6),
            (start + 3, start, start + 4, start + 7),
        ]

    def octa_head(self, cx, cy, cz, rx, ry, rz):
        start = len(self.vertices) + 1
        self.vertices += [
            (cx, cy, cz + rz), (cx, cy, cz - rz),
            (cx + rx, cy, cz), (cx, cy + ry, cz),
            (cx - rx, cy, cz), (cx, cy - ry, cz),
        ]
        self.faces += [
            (start, start + 2, start + 3), (start, start + 3, start + 4),
            (start, start + 4, start + 5), (start, start + 5, start + 2),
            (start + 1, start + 3, start + 2), (start + 1, start + 4, start + 3),
            (start + 1, start + 5, start + 4), (start + 1, start + 2, start + 5),
        ]


def main():
    mesh = ObjBuilder()
    # A readable 166 cm upright silhouette, deliberately low-poly and one draw mesh.
    mesh.box(-12, 0, 40, 18, 20, 80)  # left leg
    mesh.box(12, 0, 40, 18, 20, 80)   # right leg
    mesh.box(0, 0, 108, 42, 24, 56)   # torso
    mesh.box(0, 0, 132, 54, 25, 16)   # shoulders
    mesh.box(-31, 0, 108, 16, 18, 52) # left arm
    mesh.box(31, 0, 108, 16, 18, 52)  # right arm
    mesh.box(0, 0, 141, 12, 12, 8)    # neck
    mesh.octa_head(0, 0, 154, 14, 13, 15)

    lines = ["# AFTERSHOCK authored low-poly static citizen proxy (centimeters)", "o SM_CitizenProxy", "vt 0.0 0.0"]
    lines.extend("v %.4f %.4f %.4f" % vertex for vertex in mesh.vertices)
    lines.extend("f " + " ".join("%d/1" % index for index in face) for face in mesh.faces)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="ascii")
    print("Wrote %s (%d vertices, %d faces)" % (OUTPUT, len(mesh.vertices), len(mesh.faces)))


if __name__ == "__main__":
    main()
