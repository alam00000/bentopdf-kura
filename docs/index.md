---
layout: home

hero:
  name: Kura
  text: The open source PDF standards and preflight engine
  tagline: Convert any PDF to PDF/A, PDF/UA, PDF/X, PDF/E or PDF/VT, check it against the standard, run print preflight, and build e-invoices. One engine, from the browser to your server.
  image:
    src: /images/logo.svg
    alt: Kura
  actions:
    - theme: brand
      text: Get Started
      link: /getting-started
    - theme: alt
      text: Convert a PDF now
      link: https://kura.bentopdf.com
    - theme: alt
      text: View on GitHub
      link: https://github.com/alam00000/bentopdf-kura

features:
  - title: Honest output
    details: A file either conforms or the engine tells you why it cannot. Kura checks its own output, and a veraPDF-validated sample of every benchmark run is published, misses included.
  - title: Every standard
    details: All eleven PDF/A levels, PDF/UA-1 and UA-2, PDF/X-1a to X-6, PDF/E-1, PDF/VT, and Factur-X, ZUGFeRD, XRechnung and Order-X e-invoices. Twenty-five targets, one set of options.
  - title: Print preflight
    details: 396 bundled profiles check and repair what a press cares about, such as hairlines, rich black, low-resolution images, overprint, transparency, page boxes and fonts, and you can write your own in JSON.
  - title: Content preserving
    details: Text stays searchable, attachments stay attached, colour is converted through real ICC profiles, and a page is rasterized only when a rule leaves no other way.
  - title: Runs everywhere
    details: The same engine powers the CLI, the C library, the npm package, the Docker image and the browser build, which runs entirely on your machine.
---

## What is Kura

Kura is the PDF standards and preflight engine behind [BentoPDF](https://www.bentopdf.com). You hand it a PDF and a target standard; it parses the file, repairs what is wrong, embeds what needs embedding, converts colour where the standard demands it, and writes back a file that validates clean, or refuses with a specific reason.

It validates against every public conformance suite: the veraPDF corpus, Isartor, BFO, the Ghent Output Suite, the PDF/UA reference files and the Cal Poly PDF/VT set. A published [benchmark](/benchmark) runs it over 30,677 real-world files, every one listed with its origin, and reports crashes, timeouts, rejections and a veraPDF-validated sample of the outputs.

## How do you want to use Kura?

| You want to | Use |
|---|---|
| See how it holds up on real files | [Benchmark](/benchmark) |
| Convert or check a PDF right now, in the browser | [kura.bentopdf.com](https://kura.bentopdf.com) |
| Run print preflight on a PDF, in the browser | [kura.bentopdf.com/preflight.html](https://kura.bentopdf.com/preflight.html) |
| Convert PDFs from the terminal | the [CLI](/cli) |
| Convert PDFs from Node.js, or in your own web app | the [npm package](/npm) |
| Run conversions in a container | the [Docker image](/getting-started#in-a-container) |
| Embed the engine in another language | the [C API](/c-api) |
| Build or validate a Factur-X, ZUGFeRD or XRechnung invoice | [E-invoices](/e-invoices) |
| Run print or archival checks with your own rules | [Preflight profiles](/preflight) |
