#!/usr/bin/env python3
import os
import sys

def write_pdf(path, objs, root=1):
    out = "%PDF-1.7\n"
    off = {}
    for n, body in sorted(objs):
        off[n] = len(out)
        out += f"{n} 0 obj\n{body}\nendobj\n"
    xref = len(out)
    mx = max(off)
    out += f"xref\n0 {mx+1}\n0000000000 65535 f \n"
    for n in range(1, mx + 1):
        out += (f"{off[n]:010d} 00000 n \n" if n in off else "0000000000 00000 f \n")
    out += f"trailer\n<< /Size {mx+1} /Root {root} 0 R >>\nstartxref\n{xref}\n%%EOF\n"
    with open(path, "wb") as fh:
        fh.write(out.encode("latin1"))

def minimal_text(path):
    o = [
        (1, "<< /Type /Catalog /Pages 2 0 R >>"),
        (2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        (3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"),
        (4, "<< /Length 44 >>\nstream\nBT /F1 24 Tf 72 700 Td (Hello) Tj ET\nendstream"),
        (5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"),
    ]
    write_pdf(path, o)

def deep_pattern(path, n=8000):
    o = [(1, "<< /Type /Catalog /Pages 2 0 R >>"),
         (2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
         (3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Pattern << /P 10 0 R >> >> /Contents 4 0 R >>"),
         (4, "<< /Length 4 >>\nstream\n /P \nendstream")]
    for i in range(n):
        res = (f"/Resources << /Pattern << /P {11+i} 0 R >> >>" if i < n - 1
               else "/Resources << >>")
        o.append((10 + i, f"<< /PatternType 1 /PaintType 1 /TilingType 1 "
                          f"/BBox [0 0 1 1] /XStep 1 /YStep 1 {res} /Length 0 >>\n"
                          f"stream\n\nendstream"))
    write_pdf(path, o)

def deep_form(path, n=8000):
    o = [(1, "<< /Type /Catalog /Pages 2 0 R >>"),
         (2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
         (3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /XObject << /Fm 10 0 R >> >> /Contents 4 0 R >>"),
         (4, "<< /Length 8 >>\nstream\n/Fm Do\nendstream")]
    for i in range(n):
        res = (f"/Resources << /XObject << /Fm {11+i} 0 R >> >>" if i < n - 1
               else "/Resources << >>")
        o.append((10 + i, f"<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] "
                          f"{res} /Length 0 >>\nstream\n\nendstream"))
    write_pdf(path, o)

def deep_acroform(path, n=8000):
    o = [(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [10 0 R] >> >>"),
         (2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
         (3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>")]
    for i in range(n):
        kids = f"/Kids [{11+i} 0 R]" if i < n - 1 else ""
        o.append((10 + i, f"<< /FT /Btn /T (f{i}) {kids} >>"))
    write_pdf(path, o)

def deep_dict(path, n=6000):
    inner = "<< /Leaf 1 >>"
    for _ in range(n):
        inner = f"<< /D {inner} >>"
    o = [(1, "<< /Type /Catalog /Pages 2 0 R >>"),
         (2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
         (3, f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Nest {inner} >>")]
    write_pdf(path, o)

def profile_seeds(d):
    open(os.path.join(d, "flat.xml"), "wb").write(
        b'<?xml version="1.0"?><pdfpreflight><profile><name>p</name>'
        b'<check name="PRCWzPage_OnePageEmpty" check_severity="0"></check>'
        b'</profile></pdfpreflight>')
    open(os.path.join(d, "deep_ruleset.xml"), "wb").write(
        b'<?xml version="1.0"?><pdfpreflight>' + b'<ruleset><id1>a</id1>' * 400 +
        b'</ruleset>' * 400 + b'</pdfpreflight>')
    open(os.path.join(d, "unterminated.xml"), "wb").write(
        b'<?xml version="1.0"?><pdfpreflight><profile><name>x' + b'A' * 4096)
    open(os.path.join(d, "flat.json"), "wb").write(
        b'{"name":"p","checks":[{"name":"c","severity":"error","atoms":'
        b'[{"property":"CSGST_F::TotalAmountOfInk","op":">","value":300}]}]}')
    open(os.path.join(d, "deep.json"), "wb").write(
        b'{"a":' * 300 + b'1' + b'}' * 300)

def invoice_seeds(d):
    open(os.path.join(d, "cii.xml"), "wb").write(
        b'<?xml version="1.0" encoding="UTF-8"?>'
        b'<rsm:CrossIndustryInvoice xmlns:rsm="urn:un:unece:uncefact:data:standard:'
        b'CrossIndustryInvoice:100"><rsm:ExchangedDocument><ram:ID>INV-1</ram:ID>'
        b'</rsm:ExchangedDocument></rsm:CrossIndustryInvoice>')
    open(os.path.join(d, "nested.xml"), "wb").write(
        b'<?xml version="1.0"?>' + b'<a>' * 2000 + b'x' + b'</a>' * 2000)
    open(os.path.join(d, "truncated.xml"), "wb").write(b'<?xml version="1.0"?><rsm:Cross')

def icc_seeds(d):
    hdr = bytearray(132)
    hdr[0:4] = (128).to_bytes(4, "big")
    hdr[12:16] = b"mntr"
    hdr[16:20] = b"CMYK"
    hdr[20:24] = b"Lab "
    hdr[36:40] = b"acsp"
    open(os.path.join(d, "header_only.icc"), "wb").write(bytes(hdr))
    open(os.path.join(d, "truncated.icc"), "wb").write(bytes(hdr[:64]))
    bogus = bytearray(hdr)
    bogus[0:4] = (0xFFFFFFFF).to_bytes(4, "big")
    open(os.path.join(d, "huge_size.icc"), "wb").write(bytes(bogus))

def password_seeds(d):
    open(os.path.join(d, "empty.bin"), "wb").write(b"\x00%PDF-1.7\n%%EOF\n")
    open(os.path.join(d, "long.bin"), "wb").write(b"\x40" + b"p" * 64 + b"%PDF-1.7\n%%EOF\n")

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "build-fuzz", "seeds")
    os.makedirs(outdir, exist_ok=True)
    p = lambda name: os.path.join(outdir, name)
    minimal_text(p("minimal_text.pdf"))
    deep_pattern(p("deep_pattern.pdf"))
    deep_form(p("deep_form.pdf"))
    deep_acroform(p("deep_acroform.pdf"))
    deep_dict(p("deep_dict.pdf"))
    profile_seeds(outdir)
    invoice_seeds(outdir)
    icc_seeds(outdir)
    password_seeds(outdir)
    print(f"seeds written to {outdir}: {sorted(os.listdir(outdir))}")

if __name__ == "__main__":
    main()
