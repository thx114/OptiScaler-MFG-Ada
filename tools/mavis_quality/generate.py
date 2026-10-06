"""Generate exact-match payloads from local NVIDIA DLLs; no downloads or DLL changes.
Requires NVIDIA ptxas. Run: python generate.py --ptxas <exe> --provider <dll> [<dll> ...]
MIT upstream builder attribution: framegen/dlssg/mavis/UPSTREAM.md.
"""
from pathlib import Path
import argparse, importlib.util, subprocess, tempfile


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--ptxas', type=Path, required=True)
    parser.add_argument('--provider', type=Path, nargs='+', required=True)
    args=parser.parse_args()
    args.ptxas=args.ptxas.resolve()
    repo=Path(__file__).resolve().parents[2]
    output=repo/'OptiScaler/framegen/dlssg/mavis'
    source=Path(__file__).with_name('build_thin_geometry_variants.py')
    spec=importlib.util.spec_from_file_location('variants', source)
    variants=importlib.util.module_from_spec(spec);spec.loader.exec_module(variants)
    records=[];seen=set()
    with tempfile.TemporaryDirectory(prefix='opti-quality-') as tmp:
        for provider in args.provider:
            for name,sources,original in variants.extract_kernels(provider):
                fingerprint=variants.fingerprint_elf(original)
                identity=(fingerprint,len(original))
                if identity in seen or fingerprint[1] not in (7776,3920,784) or 120 not in sources:continue
                src=Path(tmp)/(name+'.ptx');dst=Path(tmp)/(name+'.cubin')
                src.write_text(sources[120].replace('.target sm_120','.target sm_89'), encoding='ascii')
                result=subprocess.run([str(args.ptxas),'-arch=sm_89','-O3',str(src),'-o',str(dst)],capture_output=True,text=True)
                if result.returncode:
                    print(result.stderr);continue
                data=dst.read_bytes()
                if len(data)>len(original):
                    print('Oversized base kernel skipped:',name);continue
                seen.add(identity);records.append((fingerprint,len(original),data,name))
    if not records:raise RuntimeError('No compatible Blackwell kernels; generated table not written')
    lines=['// Locally generated NVIDIA payloads. Do not commit.','#pragma once',
           'static const char kCubinsBuiltFor[]="local verified DLSSG providers";',
           'struct CubinPatch { unsigned text,shared,regs,orig_size,size; const unsigned char* data; const char* what; };']
    for i,(fp,size,data,name) in enumerate(records):
        lines.append(f'static const unsigned char kernel{i}[]={{'+','.join(f'0x{b:02x}' for b in data)+'};')
    lines.append('static const CubinPatch kCubinPatches[]={')
    for i,(fp,size,data,name) in enumerate(records):
        lines.append(f'{{{fp[0]},{fp[1]},{fp[2]},{size},sizeof(kernel{i}),kernel{i},"{name}"}},')
    lines.append('};')
    (output/'blackwell_cubins.generated.hpp').write_text('\n'.join(lines),encoding='ascii')
    subprocess.run(['python',str(source),*[str(p) for p in args.provider],'--ptxas',str(args.ptxas),
        '--output',str(output/'thin_geometry_cubins.generated.hpp'),'--allow-oversized-v3'],check=True)
    print('Generated tables at', output)


if __name__=='__main__':main()
