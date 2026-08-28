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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 4
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Image below 250 ppi",
        "severity": "warning",
        "all": [
         {
          "prop": "image.ppi",
          "op": "<",
          "value": 250
         },
         {
          "prop": "image.ppi",
          "op": ">",
          "value": 0
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "fill.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "Image below 200 ppi",
        "severity": "error",
        "all": [
         {
          "prop": "image.ppi",
          "op": "<",
          "value": 200
         },
         {
          "prop": "image.ppi",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "Image between 200 and 300 ppi",
        "severity": "warning",
        "all": [
         {
          "prop": "image.ppi",
          "op": "<",
          "value": 300
         },
         {
          "prop": "image.ppi",
          "op": ">=",
          "value": 200
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "error",
        "params": {
         "count": 0
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 2
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "error",
        "params": {
         "count": 0
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 9
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 9
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 240% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 240
         }
        ]
       },
       {
        "name": "Total ink above 240% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 240
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 200
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 600
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "error",
        "params": {
         "count": 0
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 1
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.5
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.5
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 150
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 600
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 2
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 6
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "error",
        "params": {
         "count": 0
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": [
       {
        "op": "setpagebox",
        "params": [
         "TrimBox",
         "RelativeToCropBox",
         "0",
         "0",
         "0",
         "0",
         "pt"
        ]
       },
       {
        "op": "generatebleed",
        "params": [
         "Amount",
         "3",
         "mm"
        ]
       },
       {
        "op": "removepagescaling",
        "params": []
       },
       {
        "op": "increaselinewidth",
        "params": [
         "0.25",
         "",
         "pt"
        ]
       },
       {
        "op": "overprintblack",
        "params": [
         "Text"
        ]
       },
       {
        "op": "knockoutwhite",
        "params": [
         ""
        ]
       },
       {
        "op": "trappedkey",
        "params": [
         "false"
        ]
       },
       {
        "op": "removeflatness",
        "params": []
       },
       {
        "op": "removesmoothness",
        "params": []
       },
       {
        "op": "removeunnecessarytransparencygroups",
        "params": []
       },
       {
        "op": "settransparencyblendcs",
        "params": [
         "CMYK"
        ]
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
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "conformsTo",
        "severity": "error",
        "level": "x4"
       },
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "error",
        "params": {
         "count": 0
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "conformsTo",
        "severity": "error",
        "level": "x4"
       },
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 2
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "conformsTo",
        "severity": "error",
        "level": "x4"
       },
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "error",
        "params": {
         "count": 0
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "conformsTo",
        "severity": "error",
        "level": "x4"
       },
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 1
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
        "name": "Stroke thinner than 0.25 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.5
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.5
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "conformsTo",
        "severity": "error",
        "level": "x4"
       },
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 150
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 600
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 2
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
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
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 320% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Total ink above 320% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 320
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "conformsTo",
        "severity": "error",
        "level": "x4"
       },
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 6
        }
       },
       {
        "name": "spotNamesInconsistent",
        "severity": "warning"
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": []
     }
    },
    {
     "name": "GWG 2022 sheetfed offset, CMYK: check and fix",
     "description": "The Ghent Workgroup 2022 checks for this workflow, with the safe repairs applied; convert with --level x4 to reach PDF/X-4.",
     "json": {
      "kura-profile": 1,
      "name": "GWG 2022 sheetfed offset, CMYK: check and fix",
      "description": "The Ghent Workgroup 2022 checks for this workflow, with the safe repairs applied; convert with --level x4 to reach PDF/X-4.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 330% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Total ink above 330% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 330
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Object within 3 mm of the trim edge",
        "severity": "info",
        "all": [
         {
          "prop": "content.distanceInsideTrimBox",
          "op": "<",
          "value": 8.5
         }
        ],
        "scope": "trim"
       },
       {
        "name": "Annotation that prints",
        "severity": "warning",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "conformsTo",
        "severity": "error",
        "level": "x4"
       },
       {
        "name": "imageResolutionBelow",
        "severity": "error",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "encrypted",
        "severity": "error"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "pdfVersionBelow",
        "severity": "warning",
        "params": {
         "version": 1.6
        }
       },
       {
        "name": "rgbUsed",
        "severity": "error"
       },
       {
        "name": "deviceIndependentColour",
        "severity": "warning"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "error",
        "params": {
         "count": 0
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "pagesDifferInSize",
        "severity": "warning"
       },
       {
        "name": "emptyPage",
        "severity": "warning"
       }
      ],
      "fixes": [
       {
        "op": "setpagebox",
        "params": [
         "TrimBox",
         "RelativeToCropBox",
         "0",
         "0",
         "0",
         "0",
         "pt"
        ]
       },
       {
        "op": "generatebleed",
        "params": [
         "Amount",
         "3",
         "mm"
        ]
       },
       {
        "op": "removepagescaling",
        "params": []
       },
       {
        "op": "increaselinewidth",
        "params": [
         "0.25",
         "",
         "pt"
        ]
       },
       {
        "op": "overprintblack",
        "params": [
         "Text"
        ]
       },
       {
        "op": "knockoutwhite",
        "params": [
         ""
        ]
       },
       {
        "op": "trappedkey",
        "params": [
         "false"
        ]
       },
       {
        "op": "removeflatness",
        "params": []
       },
       {
        "op": "removesmoothness",
        "params": []
       },
       {
        "op": "removeunnecessarytransparencygroups",
        "params": []
       },
       {
        "op": "settransparencyblendcs",
        "params": [
         "CMYK"
        ]
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
          "prop": "paint.isSpot",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Text smaller than 6 pt",
        "severity": "info",
        "all": [
         {
          "prop": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionAbove",
        "severity": "warning",
        "params": {
         "ppi": 150
        }
       },
       {
        "name": "bitmapResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 600
        }
       },
       {
        "name": "imageResolutionBelow",
        "severity": "info",
        "params": {
         "ppi": 72
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "encrypted",
        "severity": "warning"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "uncompressedImages",
        "severity": "warning"
       }
      ],
      "fixes": []
     }
    },
    {
     "name": "Online publishing: check and fix",
     "description": "The online checks, then the safe cleanups: page scaling removed, flatness and smoothness dropped, interpolation removed, sRGB blending; convert with --image-max-ppi 150 to downsample.",
     "json": {
      "kura-profile": 1,
      "name": "Online publishing: check and fix",
      "description": "The online checks, then the safe cleanups: page scaling removed, flatness and smoothness dropped, interpolation removed, sRGB blending; convert with --image-max-ppi 150 to downsample.",
      "checks": [
       {
        "name": "Spot colour in use",
        "severity": "warning",
        "all": [
         {
          "prop": "paint.isSpot",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Text smaller than 6 pt",
        "severity": "info",
        "all": [
         {
          "prop": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionAbove",
        "severity": "warning",
        "params": {
         "ppi": 150
        }
       },
       {
        "name": "bitmapResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 600
        }
       },
       {
        "name": "imageResolutionBelow",
        "severity": "info",
        "params": {
         "ppi": 72
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "encrypted",
        "severity": "warning"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "uncompressedImages",
        "severity": "warning"
       }
      ],
      "fixes": [
       {
        "op": "removepagescaling",
        "params": []
       },
       {
        "op": "removeflatness",
        "params": []
       },
       {
        "op": "removesmoothness",
        "params": []
       },
       {
        "op": "settransparencyblendcs",
        "params": [
         "sRGB"
        ]
       },
       {
        "op": "setinitialviewdocumentoptions",
        "params": [
         "UseNone",
         "SinglePage"
        ]
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
          "prop": "paint.isSpot",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Registration colour used for content",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isRegistration",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Text smaller than 6 pt",
        "severity": "info",
        "all": [
         {
          "prop": "text.size",
          "op": "<",
          "value": 6
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         }
        ]
       },
       {
        "name": "File larger than 5 MB",
        "severity": "error",
        "all": [
         {
          "prop": "doc.fileSizeBytes",
          "op": ">",
          "value": 5242880
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionAbove",
        "severity": "warning",
        "params": {
         "ppi": 150
        }
       },
       {
        "name": "bitmapResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 600
        }
       },
       {
        "name": "imageResolutionBelow",
        "severity": "info",
        "params": {
         "ppi": 72
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "encrypted",
        "severity": "warning"
       },
       {
        "name": "damaged",
        "severity": "error"
       },
       {
        "name": "syntaxProblems",
        "severity": "error"
       },
       {
        "name": "uncompressedImages",
        "severity": "warning"
       }
      ],
      "fixes": []
     }
    }
   ]
  },
  {
   "name": "Reports",
   "profiles": [
    {
     "name": "Report hairlines",
     "description": "Finds strokes too thin to print reliably, graded by rendered width.",
     "json": {
      "kura-profile": 1,
      "name": "Report hairlines",
      "description": "Finds strokes too thin to print reliably, graded by rendered width.",
      "checks": [
       {
        "name": "Stroke thinner than 0.125 pt",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.5
         },
         {
          "prop": "stroke.width",
          "op": ">",
          "value": 0.25
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
     }
    },
    {
     "name": "Report rich black",
     "description": "Finds black objects that also carry cyan, magenta or yellow.",
     "json": {
      "kura-profile": 1,
      "name": "Report rich black",
      "description": "Finds black objects that also carry cyan, magenta or yellow.",
      "checks": [
       {
        "name": "Rich black object",
        "severity": "info",
        "all": [
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "Rich black text below 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "content.isFilled",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "White stroke",
        "severity": "info",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "content.isStroked",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "text.isInvisible",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
     }
    },
    {
     "name": "Report low resolution images",
     "description": "Reports colour and grayscale images below 300 ppi and bitmaps below 1200 ppi.",
     "json": {
      "kura-profile": 1,
      "name": "Report low resolution images",
      "description": "Reports colour and grayscale images below 300 ppi and bitmaps below 1200 ppi.",
      "checks": [],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "bitmapResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 1200
        }
       }
      ],
      "fixes": []
     }
    },
    {
     "name": "Report high resolution images",
     "description": "Reports images above 450 ppi and bitmaps above 2400 ppi, candidates for downsampling.",
     "json": {
      "kura-profile": 1,
      "name": "Report high resolution images",
      "description": "Reports images above 450 ppi and bitmaps above 2400 ppi, candidates for downsampling.",
      "checks": [],
      "builtins": [
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "bitmapResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 2400
        }
       }
      ],
      "fixes": []
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
          "prop": "image.filter",
          "op": "contains",
          "value": "DCT"
         }
        ]
       },
       {
        "name": "JPEG 2000-compressed image",
        "severity": "info",
        "all": [
         {
          "prop": "image.filter",
          "op": "contains",
          "value": "JPX"
         }
        ]
       },
       {
        "name": "CCITT fax-compressed image",
        "severity": "info",
        "all": [
         {
          "prop": "image.filter",
          "op": "contains",
          "value": "CCITT"
         }
        ]
       },
       {
        "name": "JBIG2-compressed image",
        "severity": "info",
        "all": [
         {
          "prop": "image.filter",
          "op": "contains",
          "value": "JBIG2"
         }
        ]
       },
       {
        "name": "16-bit image",
        "severity": "info",
        "all": [
         {
          "prop": "image.bitsPerComponent",
          "op": "==",
          "value": 16
         }
        ]
       },
       {
        "name": "1-bit image",
        "severity": "info",
        "all": [
         {
          "prop": "image.bitsPerComponent",
          "op": "==",
          "value": 1
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "paint.isSpot",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "spotColoursMoreThan",
        "severity": "warning",
        "params": {
         "count": 2
        }
       }
      ],
      "fixes": []
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
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Fill set to overprint",
        "severity": "info",
        "all": [
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "Soft mask in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.hasSoftMask",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
        "name": "Type 3 font in use",
        "severity": "warning",
        "all": [
         {
          "prop": "font.isType3",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Glyph falls back to .notdef",
        "severity": "error",
        "all": [
         {
          "prop": "font.notdefUsed",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Font program is not valid",
        "severity": "error",
        "all": [
         {
          "prop": "font.invalid",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Text without Unicode",
        "severity": "info",
        "all": [
         {
          "prop": "font.unicodeComplete",
          "op": "==",
          "value": false
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "fontsNotEmbedded",
        "severity": "warning"
       }
      ],
      "fixes": []
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
          "prop": "page.isRotated",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "No crop box",
        "severity": "info",
        "all": [
         {
          "prop": "page.hasCropBox",
          "op": "==",
          "value": false
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "pagesDifferInSize",
        "severity": "info"
       },
       {
        "name": "emptyPage",
        "severity": "info"
       }
      ],
      "fixes": []
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
          "prop": "fill.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       }
      ],
      "builtins": [],
      "fixes": []
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
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "stroke.width",
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
          "prop": "stroke.width",
          "op": "<=",
          "value": 0.25
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 5
         },
         {
          "prop": "text.size",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.inkCount",
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
          "prop": "text.size",
          "op": "<",
          "value": 8
         },
         {
          "prop": "text.size",
          "op": ">",
          "value": 0.01
         },
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Total ink above 300% in fills",
        "severity": "error",
        "all": [
         {
          "prop": "fill.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Total ink above 300% in strokes",
        "severity": "error",
        "all": [
         {
          "prop": "stroke.totalInk",
          "op": ">",
          "value": 300
         }
        ]
       },
       {
        "name": "Rich black on text smaller than 12 pt",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "text.size",
          "op": "<",
          "value": 12
         },
         {
          "prop": "paint.richBlackCmyPercent",
          "op": ">",
          "value": 0
         }
        ]
       },
       {
        "name": "White object set to overprint",
        "severity": "error",
        "all": [
         {
          "prop": "paint.isWhite",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Black text not set to overprint",
        "severity": "warning",
        "all": [
         {
          "prop": "content.isText",
          "op": "==",
          "value": true
         },
         {
          "prop": "paint.is100Black",
          "op": "==",
          "value": true
         },
         {
          "prop": "fill.overprint",
          "op": "==",
          "value": false
         }
        ]
       },
       {
        "name": "Transparency in use",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.transparency",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Blend mode other than Normal",
        "severity": "info",
        "all": [
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Normal"
         },
         {
          "prop": "gstate.blendMode",
          "op": "!=",
          "value": "Compatible"
         }
        ]
       },
       {
        "name": "RGB object",
        "severity": "info",
        "all": [
         {
          "prop": "paint.isRgb",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Spot object",
        "severity": "info",
        "all": [
         {
          "prop": "paint.isSpot",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Invisible text",
        "severity": "info",
        "all": [
         {
          "prop": "text.isInvisible",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Content on a layer",
        "severity": "info",
        "all": [
         {
          "prop": "layers.onLayer",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Annotation that prints",
        "severity": "info",
        "all": [
         {
          "prop": "annot.prints",
          "op": "==",
          "value": true
         }
        ]
       },
       {
        "name": "Rotated page",
        "severity": "info",
        "all": [
         {
          "prop": "page.isRotated",
          "op": "==",
          "value": true
         }
        ]
       }
      ],
      "builtins": [
       {
        "name": "imageResolutionBelow",
        "severity": "warning",
        "params": {
         "ppi": 300
        }
       },
       {
        "name": "imageResolutionAbove",
        "severity": "info",
        "params": {
         "ppi": 450
        }
       },
       {
        "name": "fontsNotEmbedded",
        "severity": "error"
       },
       {
        "name": "spotColoursMoreThan",
        "severity": "info",
        "params": {
         "count": 2
        }
       },
       {
        "name": "pagesDifferInSize",
        "severity": "info"
       },
       {
        "name": "emptyPage",
        "severity": "info"
       },
       {
        "name": "encrypted",
        "severity": "info"
       },
       {
        "name": "damaged",
        "severity": "warning"
       }
      ],
      "fixes": []
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
      "builtins": [],
      "fixes": [
       {
        "op": "setpagebox",
        "params": [
         "TrimBox",
         "RelativeToCropBox",
         "0",
         "0",
         "0",
         "0",
         "pt"
        ]
       },
       {
        "op": "generatebleed",
        "params": [
         "Amount",
         "3",
         "mm"
        ]
       },
       {
        "op": "removepagescaling",
        "params": []
       },
       {
        "op": "increaselinewidth",
        "params": [
         "0.25",
         "",
         "pt"
        ]
       },
       {
        "op": "overprintblack",
        "params": [
         "Text"
        ]
       },
       {
        "op": "knockoutwhite",
        "params": [
         ""
        ]
       },
       {
        "op": "trappedkey",
        "params": [
         "false"
        ]
       },
       {
        "op": "removeflatness",
        "params": []
       },
       {
        "op": "removesmoothness",
        "params": []
       },
       {
        "op": "removeunnecessarytransparencygroups",
        "params": []
       },
       {
        "op": "settransparencyblendcs",
        "params": [
         "CMYK"
        ]
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
      "builtins": [],
      "fixes": [
       {
        "op": "increaselinewidth",
        "params": [
         "0.25",
         "",
         "pt"
        ]
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
      "builtins": [],
      "fixes": [
       {
        "op": "overprintblack",
        "params": [
         "Text"
        ]
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
      "builtins": [],
      "fixes": [
       {
        "op": "knockoutwhite",
        "params": [
         ""
        ]
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
      "builtins": [],
      "fixes": [
       {
        "op": "setpagebox",
        "params": [
         "TrimBox",
         "RelativeToCropBox",
         "0",
         "0",
         "0",
         "0",
         "pt"
        ]
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
      "builtins": [],
      "fixes": [
       {
        "op": "generatebleed",
        "params": [
         "Amount",
         "3",
         "mm"
        ]
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
      "builtins": [],
      "fixes": [
       {
        "op": "dscdhdnlycntfltnvsblyrs",
        "params": []
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
      "builtins": [],
      "fixes": [
       {
        "op": "settextrendermode",
        "params": [
         "0"
        ]
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
      "builtins": [],
      "fixes": [
       {
        "op": "scalepagesex",
        "params": [
         "210",
         "297",
         "mm"
        ]
       }
      ]
     }
    },
    {
     "name": "Scale pages to US Letter",
     "description": "Scales every page proportionally to fit US Letter (8.5 x 11 inch).",
     "json": {
      "kura-profile": 1,
      "name": "Scale pages to US Letter",
      "description": "Scales every page proportionally to fit US Letter (8.5 x 11 inch).",
      "checks": [],
      "builtins": [],
      "fixes": [
       {
        "op": "scalepagesex",
        "params": [
         "8.5",
         "11",
         "inch"
        ]
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
