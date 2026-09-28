"""Guard the shared C++/HLSL scalar ABI, including reserved residual slots."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def members(text):
    text = re.sub(r"//[^\n]*", "", text)
    return [(kind, name) for kind, name in
            re.findall(r"\b(uint32_t|uint|float)\s+(\w+)\s*;", text)]


class ConstantsLayoutTest(unittest.TestCase):
    def test_main_and_residual_shader_prefixes_match_cpu(self):
        header = (ROOT / "OptiScaler/shaders/dlssnr/DlssNr_Common.h").read_text(encoding="utf-8")
        cpu = members(header.split("struct alignas(256) DlssNrConstants", 1)[1].split("};", 1)[0])
        for shader in ("dlssnr.hlsl", "dlssnr_residual.hlsl"):
            with self.subTest(shader=shader):
                src = (ROOT / "OptiScaler/shaders/dlssnr/precompile" / shader).read_text(encoding="utf-8")
                gpu = members(src.split("#endif", 1)[1].split("};", 1)[0])
                self.assertGreater(len(gpu), 28)
                for i, (kind, name) in enumerate(gpu):
                    self.assertEqual(name, "g" + cpu[i][1], f"{shader}: offset {i * 4}")
                    self.assertEqual(kind, "uint" if cpu[i][0] == "uint32_t" else cpu[i][0])
        offsets = {name: i * 4 for i, (_, name) in enumerate(cpu)}
        self.assertEqual(offsets["ClampMerge"], 132)
        self.assertEqual(offsets["SourceContentWidth"], 136)
        self.assertEqual(offsets["ModelContentWidth"], 144)


if __name__ == "__main__":
    unittest.main()

