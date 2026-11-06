export const PROFILES = {
 "categories": [
  {
   "name": "Before press",
   "profiles": [
    {
     "name": "Prepress basics",
     "description": "First-pass checks a prepress operator runs before anything else.",
     "json": {
      "kura-profile": 1,
      "name": "Prepress basics",
      "description": "First-pass checks a prepress operator runs before anything else.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 4 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 4
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Image below 250 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 250,
          "kind": "colour_or_grey"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Digital print quality",
     "description": "The checks a short-run digital job usually fails: soft images, weak lines, heavy black.",
     "json": {
      "kura-profile": 1,
      "name": "Digital print quality",
      "description": "The checks a short-run digital job usually fails: soft images, weak lines, heavy black.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "Image below 200 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 200,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Image between 200 and 300 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300
         },
         {
          "check": "image.resolution",
          "op": ">=",
          "value": 200
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Sheetfed offset, CMYK: check",
     "description": "Checks a file bound for sheetfed offset on coated or uncoated stock, four process inks only: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Sheetfed offset, CMYK: check",
      "description": "Checks a file bound for sheetfed offset on coated or uncoated stock, four process inks only: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 0 spot colours on a page",
        "severity": "error",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 0,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Sheetfed offset with spot colours: check",
     "description": "Checks a file bound for sheetfed offset with up to two spot inks alongside CMYK: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Sheetfed offset with spot colours: check",
      "description": "Checks a file bound for sheetfed offset with up to two spot inks alongside CMYK: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 2 spot colours on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 2,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Web offset, heatset: check",
     "description": "Checks a file bound for heatset web offset for magazines and catalogues: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Web offset, heatset: check",
      "description": "Checks a file bound for heatset web offset for magazines and catalogues: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 0 spot colours on a page",
        "severity": "error",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 0,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Newspaper, coldset web: check",
     "description": "Checks a file bound for coldset web offset on newsprint: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Newspaper, coldset web: check",
      "description": "Checks a file bound for coldset web offset on newsprint: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 6 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 9 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 9
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 9 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 9
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 240% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 240,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 240% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 240,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 200 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 200,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 600 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 600,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 300 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 0 spot colours on a page",
        "severity": "error",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 0,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Digital print, toner: check",
     "description": "Checks a file bound for toner-based digital presses for short runs: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Digital print, toner: check",
      "description": "Checks a file bound for toner-based digital presses for short runs: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 1 spot colour on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 1,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Large format: check",
     "description": "Checks a file bound for large-format inkjet for posters, banners and displays: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Large format: check",
      "description": "Checks a file bound for large-format inkjet for posters, banners and displays: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.25 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         }
        ]
       },
       {
        "name": "Stroke between 0.25 and 0.5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.5
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.25
         }
        ]
       },
       {
        "name": "Thin stroke below 0.5 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.5
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 12 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 150 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 150,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 600 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 600,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 300 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 2 spot colours on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 2,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Packaging with spot colours: check",
     "description": "Checks a file bound for packaging with brand spot inks alongside CMYK: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Packaging with spot colours: check",
      "description": "Checks a file bound for packaging with brand spot inks alongside CMYK: resolution, ink, hairlines, small text, fonts, colour, pages and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 6 spot colours on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 6,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Sheetfed offset, CMYK: check and fix",
     "description": "The same checks for sheetfed offset on coated or uncoated stock, four process inks only, with the safe repairs applied: trim and bleed boxes, hairlines, black overprint, white knockout, page scaling and transparency groups.",
     "json": {
      "kura-profile": 1,
      "name": "Sheetfed offset, CMYK: check and fix",
      "description": "The same checks for sheetfed offset on coated or uncoated stock, four process inks only, with the safe repairs applied: trim and bleed boxes, hairlines, black overprint, white knockout, page scaling and transparency groups.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 0 spot colours on a page",
        "severity": "error",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 0,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ],
      "fixes": [
       {
        "fix": "page.set_box",
        "box": "trim",
        "from": "crop",
        "when": "missing"
       },
       {
        "fix": "bleed.generate",
        "amount": "3mm"
       },
       {
        "fix": "page.remove_user_unit"
       },
       {
        "fix": "ink.min_line_width",
        "width": "0.25pt"
       },
       {
        "fix": "ink.overprint_black",
        "objects": "text"
       },
       {
        "fix": "ink.knockout_white"
       },
       {
        "fix": "document.set_info",
        "key": "Trapped",
        "value": "False"
       },
       {
        "fix": "graphics.remove",
        "settings": [
         "flatness",
         "smoothness"
        ]
       },
       {
        "fix": "transparency.remove_unneeded_groups"
       },
       {
        "fix": "transparency.set_blend_space",
        "space": "CMYK"
       }
      ]
     }
    }
   ]
  },
  {
   "name": "Ghent Workgroup 2022",
   "profiles": [
    {
     "name": "GWG 2022 sheetfed offset, CMYK: check",
     "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 sheetfed offset, CMYK: check",
      "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
      "checks": [
       {
        "name": "Not PDF/X-4",
        "severity": "error",
        "all": [
         {
          "check": "conformance.meets",
          "op": "is",
          "value": false,
          "standard": "pdfx-4"
         }
        ]
       },
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 0 spot colours on a page",
        "severity": "error",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 0,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "GWG 2022 sheetfed offset with spot colours: check",
     "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 sheetfed offset with spot colours: check",
      "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
      "checks": [
       {
        "name": "Not PDF/X-4",
        "severity": "error",
        "all": [
         {
          "check": "conformance.meets",
          "op": "is",
          "value": false,
          "standard": "pdfx-4"
         }
        ]
       },
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 2 spot colours on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 2,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "GWG 2022 heatset web offset: check",
     "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 heatset web offset: check",
      "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
      "checks": [
       {
        "name": "Not PDF/X-4",
        "severity": "error",
        "all": [
         {
          "check": "conformance.meets",
          "op": "is",
          "value": false,
          "standard": "pdfx-4"
         }
        ]
       },
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 0 spot colours on a page",
        "severity": "error",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 0,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "GWG 2022 digital print: check",
     "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 digital print: check",
      "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
      "checks": [
       {
        "name": "Not PDF/X-4",
        "severity": "error",
        "all": [
         {
          "check": "conformance.meets",
          "op": "is",
          "value": false,
          "standard": "pdfx-4"
         }
        ]
       },
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 1 spot colour on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 1,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "GWG 2022 large format: check",
     "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 large format: check",
      "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
      "checks": [
       {
        "name": "Not PDF/X-4",
        "severity": "error",
        "all": [
         {
          "check": "conformance.meets",
          "op": "is",
          "value": false,
          "standard": "pdfx-4"
         }
        ]
       },
       {
        "name": "Stroke thinner than 0.25 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         }
        ]
       },
       {
        "name": "Stroke between 0.25 and 0.5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.5
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.25
         }
        ]
       },
       {
        "name": "Thin stroke below 0.5 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.5
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 12 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 150 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 150,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 600 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 600,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 300 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 2 spot colours on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 2,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "GWG 2022 packaging with spot colours: check",
     "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 packaging with spot colours: check",
      "description": "Written from the Ghent Workgroup 2022 specification for this workflow: PDF/X-4 conformance plus the workflow's resolution, ink, hairline, text, font, colour and page requirements.",
      "checks": [
       {
        "name": "Not PDF/X-4",
        "severity": "error",
        "all": [
         {
          "check": "conformance.meets",
          "op": "is",
          "value": false,
          "standard": "pdfx-4"
         }
        ]
       },
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 320,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 6 spot colours on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 6,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Spot colour names that differ only in spelling",
        "severity": "warning",
        "all": [
         {
          "check": "print.spot_naming",
          "op": "includes",
          "value": "similar_names"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "GWG 2022 sheetfed offset, CMYK: check and fix",
     "description": "The Ghent Workgroup 2022 checks for this workflow, with the safe repairs applied, converted to PDF/X-4.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 sheetfed offset, CMYK: check and fix",
      "description": "The Ghent Workgroup 2022 checks for this workflow, with the safe repairs applied, converted to PDF/X-4.",
      "convert": {
       "level": "pdfx-4"
      },
      "checks": [
       {
        "name": "Not PDF/X-4",
        "severity": "error",
        "all": [
         {
          "check": "conformance.meets",
          "op": "is",
          "value": false,
          "standard": "pdfx-4"
         }
        ]
       },
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 330,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "error",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "error",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "PDF version below 1.6",
        "severity": "warning",
        "all": [
         {
          "check": "document.pdf_version",
          "op": "<",
          "value": 1.6
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "RGB colour",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "color.space",
            "op": "in",
            "value": [
             "DeviceRGB",
             "CalRGB"
            ]
           }
          ]
         },
         {
          "all": [
           {
            "check": "color.space",
            "op": "==",
            "value": "ICCBased",
            "icc_components": 3
           }
          ]
         }
        ]
       },
       {
        "name": "Device-independent colour",
        "severity": "warning",
        "all": [
         {
          "check": "color.space",
          "op": "in",
          "value": [
           "device_independent"
          ]
         }
        ]
       },
       {
        "name": "More than 0 spot colours on a page",
        "severity": "error",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 0,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "warning",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "warning",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "error",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "check": "content.distance_to_box",
          "op": "<",
          "value": 3,
          "box": "trim",
          "side": "inside",
          "unit": "mm"
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       }
      ],
      "fixes": [
       {
        "fix": "page.set_box",
        "box": "trim",
        "from": "crop",
        "when": "missing"
       },
       {
        "fix": "bleed.generate",
        "amount": "3mm"
       },
       {
        "fix": "page.remove_user_unit"
       },
       {
        "fix": "ink.min_line_width",
        "width": "0.25pt"
       },
       {
        "fix": "ink.overprint_black",
        "objects": "text"
       },
       {
        "fix": "ink.knockout_white"
       },
       {
        "fix": "document.set_info",
        "key": "Trapped",
        "value": "False"
       },
       {
        "fix": "graphics.remove",
        "settings": [
         "flatness",
         "smoothness"
        ]
       },
       {
        "fix": "transparency.remove_unneeded_groups"
       },
       {
        "fix": "transparency.set_blend_space",
        "space": "CMYK"
       }
      ]
     }
    }
   ]
  },
  {
   "name": "Online publishing",
   "profiles": [
    {
     "name": "Online publishing: check",
     "description": "Checks a file meant for screens and downloads: images above 150 ppi that add weight, fonts, spot colours, encryption and syntax.",
     "json": {
      "kura-profile": 1,
      "name": "Online publishing: check",
      "description": "Checks a file meant for screens and downloads: images above 150 ppi that add weight, fonts, spot colours, encryption and syntax.",
      "checks": [
       {
        "name": "Spot colour in use",
        "severity": "warning",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "in",
          "value": [
           "spot_only",
           "spot_and_black",
           "process_and_spot"
          ]
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "Text smaller than 6 pt",
        "severity": "info",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "warning",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Image above 150 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 150,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap above 600 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 600,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image below 72 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 72,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "warning",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "Uncompressed image",
        "severity": "warning",
        "all": [
         {
          "check": "image.property",
          "op": "==",
          "value": "false",
          "property": "compressed"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Online publishing: check and fix",
     "description": "The online checks, then the safe cleanups: page scaling removed, flatness and smoothness dropped, colour images above 225 ppi brought down to 150 ppi, sRGB blending, single-page view.",
     "json": {
      "kura-profile": 1,
      "name": "Online publishing: check and fix",
      "description": "The online checks, then the safe cleanups: page scaling removed, flatness and smoothness dropped, colour images above 225 ppi brought down to 150 ppi, sRGB blending, single-page view.",
      "checks": [
       {
        "name": "Spot colour in use",
        "severity": "warning",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "in",
          "value": [
           "spot_only",
           "spot_and_black",
           "process_and_spot"
          ]
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "Text smaller than 6 pt",
        "severity": "info",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "warning",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Image above 150 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 150,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap above 600 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 600,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image below 72 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 72,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "warning",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "Uncompressed image",
        "severity": "warning",
        "all": [
         {
          "check": "image.property",
          "op": "==",
          "value": "false",
          "property": "compressed"
         }
        ]
       }
      ],
      "fixes": [
       {
        "fix": "page.remove_user_unit"
       },
       {
        "fix": "graphics.remove",
        "settings": [
         "flatness",
         "smoothness"
        ]
       },
       {
        "fix": "image.downsample",
        "kind": "colour_and_grey",
        "target": "150ppi",
        "above": "225ppi"
       },
       {
        "fix": "transparency.set_blend_space",
        "space": "sRGB"
       },
       {
        "fix": "document.set_view",
        "page_mode": "UseNone",
        "page_layout": "SinglePage"
       }
      ]
     }
    },
    {
     "name": "Email attachment: check",
     "description": "Everything in the online check plus a 5 MB size limit.",
     "json": {
      "kura-profile": 1,
      "name": "Email attachment: check",
      "description": "Everything in the online check plus a 5 MB size limit.",
      "checks": [
       {
        "name": "Spot colour in use",
        "severity": "warning",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "in",
          "value": [
           "spot_only",
           "spot_and_black",
           "process_and_spot"
          ]
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "registration"
         }
        ]
       },
       {
        "name": "Text smaller than 6 pt",
        "severity": "info",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "warning",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "Image above 150 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 150,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap above 600 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 600,
          "kind": "bitmap"
         }
        ]
       },
       {
        "name": "Image below 72 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 72,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "warning",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "error",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Syntax problem",
        "severity": "error",
        "any": [
         {
          "all": [
           {
            "check": "syntax.content_stream_error",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.xref_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.object_layout",
            "op": "count",
            "value": 0,
            "compare": ">"
           }
          ]
         },
         {
          "all": [
           {
            "check": "syntax.broken_reference",
            "op": "in",
            "value": [
             "object",
             "external_file"
            ]
           }
          ]
         }
        ]
       },
       {
        "name": "Uncompressed image",
        "severity": "warning",
        "all": [
         {
          "check": "image.property",
          "op": "==",
          "value": "false",
          "property": "compressed"
         }
        ]
       },
       {
        "name": "File larger than 5 MB",
        "severity": "error",
        "all": [
         {
          "check": "document.file_size",
          "op": ">",
          "value": 5,
          "unit": "mb"
         }
        ]
       }
      ]
     }
    }
   ]
  },
  {
   "name": "Reports",
   "profiles": [
    {
     "name": "Report hairlines",
     "description": "Finds strokes too thin to print reliably, graded by width.",
     "json": {
      "kura-profile": 1,
      "name": "Report hairlines",
      "description": "Finds strokes too thin to print reliably, graded by width.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Stroke between 0.25 and 0.5 pt",
        "severity": "info",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.5
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.25
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report small text",
     "description": "Reports text at sizes that print or read poorly.",
     "json": {
      "kura-profile": 1,
      "name": "Report small text",
      "description": "Reports text at sizes that print or read poorly.",
      "checks": [
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report rich black",
     "description": "Finds solid black objects that also carry cyan, magenta or yellow.",
     "json": {
      "kura-profile": 1,
      "name": "Report rich black",
      "description": "Finds solid black objects that also carry cyan, magenta or yellow.",
      "checks": [
       {
        "name": "Rich black object",
        "severity": "info",
        "all": [
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "Rich black text below 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report white objects",
     "description": "Finds white fills and strokes, which either knock out or vanish when overprinting.",
     "json": {
      "kura-profile": 1,
      "name": "Report white objects",
      "description": "Finds white fills and strokes, which either knock out or vanish when overprinting.",
      "checks": [
       {
        "name": "White fill",
        "severity": "info",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "content.painting",
          "op": "in",
          "value": [
           "fill",
           "fill_and_stroke"
          ]
         }
        ]
       },
       {
        "name": "White stroke",
        "severity": "info",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "content.painting",
          "op": "in",
          "value": [
           "stroke",
           "fill_and_stroke"
          ]
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report invisible text",
     "description": "Finds text drawn in invisible rendering mode, typically OCR layers.",
     "json": {
      "kura-profile": 1,
      "name": "Report invisible text",
      "description": "Finds text drawn in invisible rendering mode, typically OCR layers.",
      "checks": [
       {
        "name": "Invisible text",
        "severity": "info",
        "all": [
         {
          "check": "text.rendering",
          "op": "==",
          "value": "invisible"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report low resolution images",
     "description": "Reports colour and grayscale images below 300 ppi and bitmaps below 1200 ppi.",
     "json": {
      "kura-profile": 1,
      "name": "Report low resolution images",
      "description": "Reports colour and grayscale images below 300 ppi and bitmaps below 1200 ppi.",
      "checks": [
       {
        "name": "Image below 300 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap below 1200 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 1200,
          "kind": "bitmap"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report high resolution images",
     "description": "Reports images above 450 ppi and bitmaps above 2400 ppi, candidates for downsampling.",
     "json": {
      "kura-profile": 1,
      "name": "Report high resolution images",
      "description": "Reports images above 450 ppi and bitmaps above 2400 ppi, candidates for downsampling.",
      "checks": [
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Bitmap above 2400 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 2400,
          "kind": "bitmap"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report image formats",
     "description": "Reports the compression filters and bit depths in use.",
     "json": {
      "kura-profile": 1,
      "name": "Report image formats",
      "description": "Reports the compression filters and bit depths in use.",
      "checks": [
       {
        "name": "JPEG-compressed image",
        "severity": "info",
        "all": [
         {
          "check": "image.property",
          "op": "contains",
          "value": "DCTDecode",
          "property": "filter"
         }
        ]
       },
       {
        "name": "JPEG 2000-compressed image",
        "severity": "info",
        "all": [
         {
          "check": "image.property",
          "op": "contains",
          "value": "JPXDecode",
          "property": "filter"
         }
        ]
       },
       {
        "name": "CCITT fax-compressed image",
        "severity": "info",
        "all": [
         {
          "check": "image.property",
          "op": "contains",
          "value": "CCITTFaxDecode",
          "property": "filter"
         }
        ]
       },
       {
        "name": "JBIG2-compressed image",
        "severity": "info",
        "all": [
         {
          "check": "image.property",
          "op": "contains",
          "value": "JBIG2Decode",
          "property": "filter"
         }
        ]
       },
       {
        "name": "16-bit image",
        "severity": "info",
        "all": [
         {
          "check": "image.property",
          "op": "==",
          "value": "16",
          "property": "bits_per_component"
         }
        ]
       },
       {
        "name": "1-bit image",
        "severity": "info",
        "all": [
         {
          "check": "image.property",
          "op": "==",
          "value": "1",
          "property": "bits_per_component"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report spot colours",
     "description": "Reports every spot colour object and pages with more than two spot inks.",
     "json": {
      "kura-profile": 1,
      "name": "Report spot colours",
      "description": "Reports every spot colour object and pages with more than two spot inks.",
      "checks": [
       {
        "name": "Spot object",
        "severity": "info",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "in",
          "value": [
           "spot_only",
           "spot_and_black",
           "process_and_spot"
          ]
         }
        ]
       },
       {
        "name": "More than 2 spot colours on a page",
        "severity": "warning",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 2,
          "compare": ">",
          "kind": "spot"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report overprint",
     "description": "Reports overprinting fills and strokes and white objects set to overprint.",
     "json": {
      "kura-profile": 1,
      "name": "Report overprint",
      "description": "Reports overprinting fills and strokes and white objects set to overprint.",
      "checks": [
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Fill set to overprint",
        "severity": "info",
        "all": [
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report transparency",
     "description": "Reports transparency, blend modes and soft masks.",
     "json": {
      "kura-profile": 1,
      "name": "Report transparency",
      "description": "Reports transparency, blend modes and soft masks.",
      "checks": [
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "Soft mask in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true,
          "how": "soft_mask"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report fonts",
     "description": "Reports fonts not embedded, Type 3 fonts, missing glyphs and text without Unicode.",
     "json": {
      "kura-profile": 1,
      "name": "Report fonts",
      "description": "Reports fonts not embedded, Type 3 fonts, missing glyphs and text without Unicode.",
      "checks": [
       {
        "name": "Font not embedded",
        "severity": "warning",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "check": "font.type",
          "op": "==",
          "value": "type3"
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "check": "text.glyph",
          "op": "==",
          "value": "notdef"
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "check": "font.problem",
          "op": "includes",
          "value": "invalid_program"
         }
        ]
       },
       {
        "name": "Text without Unicode",
        "severity": "info",
        "all": [
         {
          "check": "font.all_text_unicode",
          "op": "is",
          "value": false
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report page geometry",
     "description": "Reports page sizes, rotation, scaling, empty pages and boxes.",
     "json": {
      "kura-profile": 1,
      "name": "Report page geometry",
      "description": "Reports page sizes, rotation, scaling, empty pages and boxes.",
      "checks": [
       {
        "name": "Rotated page",
        "severity": "info",
        "all": [
         {
          "check": "page.rotation",
          "op": "!=",
          "value": 0
         }
        ]
       },
       {
        "name": "Page scaling in use",
        "severity": "warning",
        "all": [
         {
          "check": "page.user_unit",
          "op": "!=",
          "value": 1
         }
        ]
       },
       {
        "name": "No crop box",
        "severity": "info",
        "all": [
         {
          "check": "page.box_present",
          "op": "is",
          "value": false,
          "box": "crop"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "info",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "info",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report ink coverage",
     "description": "Reports fills and strokes above 300% total ink and rich black on text.",
     "json": {
      "kura-profile": 1,
      "name": "Report ink coverage",
      "description": "Reports fills and strokes above 300% total ink and rich black on text.",
      "checks": [
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       }
      ]
     }
    },
    {
     "name": "Report everything",
     "description": "Every report in one run: images, colour, ink, hairlines, text, transparency, fonts, pages, annotations, layers and document health.",
     "json": {
      "kura-profile": 1,
      "name": "Report everything",
      "description": "Every report in one run: images, colour, ink, hairlines, text, transparency, fonts, pages, annotations, layers and document health.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Stroke between 0.125 and 0.25 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "content.stroke_width",
          "op": ">",
          "value": 0.125
         }
        ]
       },
       {
        "name": "Thin stroke below 0.25 pt in more than one ink",
        "severity": "error",
        "all": [
         {
          "check": "content.stroke_width",
          "op": "<=",
          "value": 0.25
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "Text smaller than 5 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "Text smaller than 8 pt in more than one ink",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.inks_used",
          "op": ">",
          "value": 1
         }
        ]
       },
       {
        "name": "White text smaller than 8 pt",
        "severity": "warning",
        "all": [
         {
          "check": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "check": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "check": "color.total_ink",
          "op": ">",
          "value": 300,
          "paint": "stroke"
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "check": "color.ink",
          "op": ">=",
          "value": 100,
          "ink": "black"
         },
         {
          "check": "color.ink",
          "op": ">",
          "value": 0,
          "ink": "cmy_max"
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "none"
         },
         {
          "check": "color.overprint",
          "op": "in",
          "value": [
           "on",
           "on_nonzero_mode"
          ],
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "check": "content.kind",
          "op": "==",
          "value": "text"
         },
         {
          "check": "color.ink_mix",
          "op": "==",
          "value": "solid_black"
         },
         {
          "check": "color.overprint",
          "op": "==",
          "value": "off",
          "paint": "fill"
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "check": "transparency.used",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "check": "transparency.blend_mode",
          "op": "not_in",
          "value": [
           "Normal",
           "Compatible"
          ]
         }
        ]
       },
       {
        "name": "RGB object",
        "severity": "info",
        "all": [
         {
          "check": "color.space",
          "op": "==",
          "value": "DeviceRGB"
         }
        ]
       },
       {
        "name": "Spot object",
        "severity": "info",
        "all": [
         {
          "check": "color.ink_mix",
          "op": "in",
          "value": [
           "spot_only",
           "spot_and_black",
           "process_and_spot"
          ]
         }
        ]
       },
       {
        "name": "Invisible text",
        "severity": "info",
        "all": [
         {
          "check": "text.rendering",
          "op": "==",
          "value": "invisible"
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "check": "layer.membership",
          "op": "count",
          "value": 0,
          "compare": ">"
         }
        ]
       },
       {
        "name": "Annotation that prints",
        "severity": "info",
        "all": [
         {
          "check": "annotation.flags",
          "op": "includes",
          "value": "print"
         }
        ]
       },
       {
        "name": "Rotated page",
        "severity": "info",
        "all": [
         {
          "check": "page.rotation",
          "op": "!=",
          "value": 0
         }
        ]
       },
       {
        "name": "Image below 300 ppi",
        "severity": "warning",
        "all": [
         {
          "check": "image.resolution",
          "op": "<",
          "value": 300,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Image above 450 ppi",
        "severity": "info",
        "all": [
         {
          "check": "image.resolution",
          "op": ">",
          "value": 450,
          "kind": "colour_or_grey"
         }
        ]
       },
       {
        "name": "Font not embedded",
        "severity": "error",
        "all": [
         {
          "check": "font.embedding",
          "op": "==",
          "value": "not_embedded"
         }
        ]
       },
       {
        "name": "More than 2 spot colours on a page",
        "severity": "info",
        "all": [
         {
          "check": "print.plates",
          "op": "count",
          "value": 2,
          "compare": ">",
          "kind": "spot"
         }
        ]
       },
       {
        "name": "Pages differ in size or orientation",
        "severity": "info",
        "all": [
         {
          "check": "document.pages_uniform",
          "op": "is",
          "value": false
         }
        ]
       },
       {
        "name": "Empty page",
        "severity": "info",
        "all": [
         {
          "check": "page.empty",
          "op": "is",
          "value": true
         }
        ]
       },
       {
        "name": "Document is encrypted",
        "severity": "info",
        "all": [
         {
          "check": "security.encryption",
          "op": "!=",
          "value": "none"
         }
        ]
       },
       {
        "name": "Document is damaged",
        "severity": "warning",
        "all": [
         {
          "check": "document.damaged",
          "op": "is",
          "value": true
         }
        ]
       }
      ]
     }
    }
   ]
  },
  {
   "name": "Fixes",
   "profiles": [
    {
     "name": "Prepress cleanup",
     "description": "The safe prepress repairs in one step: trim and bleed boxes, hairlines, overprint, knockout, scaling, transparency groups.",
     "json": {
      "kura-profile": 1,
      "name": "Prepress cleanup",
      "description": "The safe prepress repairs in one step: trim and bleed boxes, hairlines, overprint, knockout, scaling, transparency groups.",
      "checks": [],
      "fixes": [
       {
        "fix": "page.set_box",
        "box": "trim",
        "from": "crop",
        "when": "missing"
       },
       {
        "fix": "bleed.generate",
        "amount": "3mm"
       },
       {
        "fix": "page.remove_user_unit"
       },
       {
        "fix": "ink.min_line_width",
        "width": "0.25pt"
       },
       {
        "fix": "ink.overprint_black",
        "objects": "text"
       },
       {
        "fix": "ink.knockout_white"
       },
       {
        "fix": "document.set_info",
        "key": "Trapped",
        "value": "False"
       },
       {
        "fix": "graphics.remove",
        "settings": [
         "flatness",
         "smoothness"
        ]
       },
       {
        "fix": "transparency.remove_unneeded_groups"
       },
       {
        "fix": "transparency.set_blend_space",
        "space": "CMYK"
       }
      ]
     }
    },
    {
     "name": "Thicken hairlines to 0.25 pt",
     "description": "Raises every stroke thinner than 0.25 pt to 0.25 pt.",
     "json": {
      "kura-profile": 1,
      "name": "Thicken hairlines to 0.25 pt",
      "description": "Raises every stroke thinner than 0.25 pt to 0.25 pt.",
      "checks": [],
      "fixes": [
       {
        "fix": "ink.min_line_width",
        "width": "0.25pt"
       }
      ]
     }
    },
    {
     "name": "Black text to overprint",
     "description": "Sets 100% black text to overprint.",
     "json": {
      "kura-profile": 1,
      "name": "Black text to overprint",
      "description": "Sets 100% black text to overprint.",
      "checks": [],
      "fixes": [
       {
        "fix": "ink.overprint_black",
        "objects": "text"
       }
      ]
     }
    },
    {
     "name": "All white objects to knockout",
     "description": "Switches overprint off for every white object.",
     "json": {
      "kura-profile": 1,
      "name": "All white objects to knockout",
      "description": "Switches overprint off for every white object.",
      "checks": [],
      "fixes": [
       {
        "fix": "ink.knockout_white",
        "objects": "all"
       }
      ]
     }
    },
    {
     "name": "Set trim box from crop box",
     "description": "Adds a trim box equal to the crop box on pages that lack one.",
     "json": {
      "kura-profile": 1,
      "name": "Set trim box from crop box",
      "description": "Adds a trim box equal to the crop box on pages that lack one.",
      "checks": [],
      "fixes": [
       {
        "fix": "page.set_box",
        "box": "trim",
        "from": "crop",
        "left": "0mm",
        "right": "0mm",
        "top": "0mm",
        "bottom": "0mm",
        "when": "missing"
       }
      ]
     }
    },
    {
     "name": "Add 3 mm bleed box",
     "description": "Sets a bleed box 3 mm outside the trim box, limited by the media box.",
     "json": {
      "kura-profile": 1,
      "name": "Add 3 mm bleed box",
      "description": "Sets a bleed box 3 mm outside the trim box, limited by the media box.",
      "checks": [],
      "fixes": [
       {
        "fix": "page.set_box",
        "box": "bleed",
        "from": "trim",
        "left": "-3mm",
        "right": "-3mm",
        "top": "-3mm",
        "bottom": "-3mm"
       }
      ]
     }
    },
    {
     "name": "Flatten layers",
     "description": "Discards hidden layers and merges the visible ones into the page.",
     "json": {
      "kura-profile": 1,
      "name": "Flatten layers",
      "description": "Discards hidden layers and merges the visible ones into the page.",
      "checks": [],
      "fixes": [
       {
        "fix": "layer.flatten"
       }
      ]
     }
    },
    {
     "name": "Make invisible text visible",
     "description": "Sets invisible text to plain fill so it shows and prints.",
     "json": {
      "kura-profile": 1,
      "name": "Make invisible text visible",
      "description": "Sets invisible text to plain fill so it shows and prints.",
      "checks": [],
      "fixes": [
       {
        "fix": "text.set_render_mode",
        "mode": "fill"
       }
      ]
     }
    },
    {
     "name": "Scale pages to A4",
     "description": "Scales every page proportionally to fit A4 (210 x 297 mm).",
     "json": {
      "kura-profile": 1,
      "name": "Scale pages to A4",
      "description": "Scales every page proportionally to fit A4 (210 x 297 mm).",
      "checks": [],
      "fixes": [
       {
        "fix": "page.scale",
        "width": "210mm",
        "height": "297mm"
       }
      ]
     }
    },
    {
     "name": "Scale pages to US Letter",
     "description": "Scales every page proportionally to fit US Letter (8.5 x 11 in).",
     "json": {
      "kura-profile": 1,
      "name": "Scale pages to US Letter",
      "description": "Scales every page proportionally to fit US Letter (8.5 x 11 in).",
      "checks": [],
      "fixes": [
       {
        "fix": "page.scale",
        "width": "8.5in",
        "height": "11in"
       }
      ]
     }
    }
   ]
  },
  {
   "name": "Archive (PDF/A)",
   "profiles": [
    {
     "name": "Convert to PDF/A-1b",
     "description": "Convert the file to the PDF/A-1b archival standard.",
     "level": "1b"
    },
    {
     "name": "Convert to PDF/A-2b",
     "description": "Convert the file to the PDF/A-2b archival standard, the usual choice.",
     "level": "2b"
    },
    {
     "name": "Convert to PDF/A-2u",
     "description": "Convert to PDF/A-2u, with every character mapped to Unicode.",
     "level": "2u"
    },
    {
     "name": "Convert to PDF/A-3b",
     "description": "Convert to PDF/A-3b, which may carry attachments of any kind.",
     "level": "3b"
    },
    {
     "name": "Convert to PDF/A-4",
     "description": "Convert to PDF/A-4, the PDF 2.0 archival standard.",
     "level": "4"
    }
   ]
  },
  {
   "name": "Accessibility (PDF/UA)",
   "profiles": [
    {
     "name": "Convert to PDF/A-2a + PDF/UA-1",
     "description": "Convert with full tagging and accessibility identification.",
     "level": "2a",
     "ua": true
    },
    {
     "name": "Convert to PDF/A-4 + PDF/UA-2",
     "description": "Convert to the PDF 2.0 archival standard with PDF/UA-2 accessibility.",
     "level": "4",
     "ua": true
    }
   ]
  },
  {
   "name": "Print standards (PDF/X)",
   "profiles": [
    {
     "name": "Convert to PDF/X-1a",
     "description": "Convert to the blind-exchange print standard (CMYK only, no transparency).",
     "level": "x1a"
    },
    {
     "name": "Convert to PDF/X-3",
     "description": "Convert to PDF/X-3, colour-managed print exchange.",
     "level": "x3"
    },
    {
     "name": "Convert to PDF/X-4",
     "description": "Convert to PDF/X-4, the modern print standard with live transparency.",
     "level": "x4"
    },
    {
     "name": "Convert to PDF/X-6",
     "description": "Convert to PDF/X-6, the PDF 2.0 print standard.",
     "level": "x6"
    }
   ]
  },
  {
   "name": "Engineering and variable data",
   "profiles": [
    {
     "name": "Convert to PDF/E-1",
     "description": "Convert to the engineering document standard.",
     "level": "e1"
    },
    {
     "name": "Convert to PDF/VT-1",
     "description": "Convert to PDF/VT-1 for variable-data print.",
     "level": "vt1"
    }
   ]
  }
 ]
};
