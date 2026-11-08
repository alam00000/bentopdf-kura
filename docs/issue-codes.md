# Issue codes

Every report lists what Kura did or found as `issues`, and, with `--analyze` or a profile, `analysis`. Each entry has a `code`, a `detail` sentence with the specifics (which font, how many objects, which pages), and these fields:

| Field | Meaning |
|---|---|
| `fixed` | `true`: Kura changed the file to fix this, or, in [check mode](/check-mode), a conversion would change it. `false`: noted, but nothing was changed |
| `severity` | `1` information, `2` warning, `3` error |
| `pages` | the 1-based pages concerned; empty means the whole document |

This page lists every code with a one-line meaning. Codes that stop a conversion are on [Rejection codes](/rejections), and profile results are explained on [Preflight profiles](/preflight).

Three codes appear on almost every conversion and are not counted as findings in check mode: `XMP_REBUILT`, `CONTENT_FILTERED` and `OUTPUT_INTENT_PRESENT`. They are routine tidying, not evidence of a defect.

## Metadata and document information

| Code | Meaning |
|---|---|
| `XMP_REBUILT` | the XMP metadata was regenerated with the standard's identification (routine) |
| `XMP_NONCONFORMING` | the existing XMP metadata broke the standard's rules; the detail says how |
| `OBJECT_METADATA_REMOVED` | XMP metadata attached to single objects used schemas the standard does not allow and was removed |
| `DOCINFO_MIGRATED` | the document information dictionary was moved into XMP, as PDF 2.0 requires |
| `DOCINFO_REMOVED` | the document information dictionary was removed (PDF 2.0 print standards) |
| `DOCINFO_REDUCED` | only the modification date was kept in the document information, as private application data needs it |
| `DOCINFO_CUSTOM_REMOVED` | an old PDF/X key was removed from the document information |
| `DOCINFO_DATE_DROPPED` | a date that could not be read was removed |
| `DOCINFO_DATE_SYNTHESIZED` | a required creation date was added |
| `PIECEINFO_REMOVED` | private application data was removed from the catalog |
| `TITLE_SYNTHESIZED` | PDF/X needs a title; "Untitled document" was set |
| `TRAPPED_SET` | PDF/X needs the trapped flag to be True or False; False was set |
| `X_VERSION_SET` | the PDF/X version marker was written or corrected |
| `VT_VERSION_SET` | the PDF/VT version marker was written or corrected |
| `LANG_SET` | the document had no valid language, so one was set (from `--lang` when given) |
| `LANG_NORMALIZED` | a language tag was rewritten in standard form |
| `LANG_REMOVED` | an invalid language tag was removed |
| `CONTENT_LANG_FIXED` | invalid language values inside page content were replaced |
| `VIEWER_PREFERENCES_FIXED` | a viewer preference that would hide part of the print area was removed |
| `BOOKMARKS_ADDED` | bookmarks were built from `--bookmarks` |
| `BOOKMARKS_NOT_CREATED` | `--bookmarks` was given but no bookmarks could be built (no tagged headings, or unreadable JSON) |

## Fonts and text

| Code | Meaning |
|---|---|
| `FONT_SUBSTITUTED` | a font that was not embedded was replaced by an embedded substitute with the same widths (warning) |
| `FONT_NOT_EMBEDDED` | a font is not embedded and no substitute applies |
| `FONT_FSTYPE_RESTRICTED` | the font's licence flags forbid embedding; a substitute was used |
| `FONT_PROGRAM_UNREADABLE` | the embedded font data could not be read |
| `FONT_PROGRAM_UNUSABLE` | the embedded font data could not be parsed, so a substitute was used |
| `FONT_PROGRAM_TRIMMED` | stray data after the end of a Type 1 font program was removed |
| `FONT_PROGRAM_LENGTHS_FIXED` | the recorded section lengths of a Type 1 font program were corrected |
| `FONT_PROGRAM_METRICS_INLINED` | glyph width instructions were moved into the glyphs themselves |
| `OPENTYPE_UNWRAPPED` | an OpenType font was stored as plain compact font data, for levels that do not allow OpenType |
| `FONT_DICT_REPAIRED` | a font dictionary lacked a required entry or named a font type that is not allowed |
| `FONT_RESOURCE_REPAIRED` | a font resource pointed straight at a CID font; it was wrapped in a composite font |
| `FONT_RESOURCE_REPLACED` | a font resource did not point at a font; a standard font was put in its place |
| `FONT_NAME_SANITIZED` | a font name that was not valid UTF-8 was renamed |
| `FONT_FLAGS_FIXED` | the symbolic and non-symbolic flags were made to match the font |
| `SYMBOLIC_FLAG_CLEARED` | a font marked symbolic was reclassified as an ordinary font |
| `FONT_CMAP_ADDED` | a Unicode character map was added inside a TrueType font |
| `FONT_WIDTHS_SYNCED` | glyph widths in the PDF were corrected to match the embedded font |
| `TYPE3_WIDTHS_SYNCED` | glyph widths of a Type 3 font were corrected to match its glyph drawings |
| `TTF_ENCODING_SET` | a standard encoding was set on a TrueType font |
| `TT_DIFFERENCES_REMOVED` | a glyph difference list that TrueType fonts may not carry was removed |
| `SYMBOLIC_ENCODING_REMOVED` | an encoding that symbolic TrueType fonts may not carry was removed |
| `ENCODING_DIFFS_CLEANED` | non-standard glyph names in an encoding were replaced or removed |
| `CHARSET_ADDED`, `CHARSET_REMOVED` | the list of glyphs in a Type 1 subset was written or removed |
| `CIDSET_ADDED`, `CIDSET_REMOVED` | the list of glyphs in a CID font subset was written or removed |
| `CIDSYSTEMINFO_SYNCED` | the character collection of a composite font was aligned with its CID font |
| `CIDTOGIDMAP_SET` | the glyph mapping of a CID font was set to identity |
| `CMAP_REPAIRED` | an embedded character map was repaired |
| `CMAP_ROS_SYNCED` | an embedded character map's collection was aligned with its font |
| `CMAP_WMODE_SYNCED` | an embedded character map's writing direction was aligned with its program |
| `CMAP_USECMAP_REMOVED` | an unused reference to another character map was removed |
| `CMAP_FALLBACK_EMBEDDED` | a character map that could not be resolved was replaced by an identity map (visual difference possible) |
| `TOUNICODE_BUILT` | the mapping from glyphs to Unicode text was completed, so text can be searched and read aloud |
| `TOUNICODE_SCRUBBED` | forbidden Unicode values were removed from a text mapping |
| `ACTUALTEXT_MISSING` | private-use characters are shown without a replacement text |
| `ACTUALTEXT_PUA_REMOVED` | private-use characters were removed from replacement text |
| `MISSING_GLYPHS_REMOVED` | text that drew glyphs absent from its font (shown as empty boxes before) was removed |
| `TEXT_OUTLINED` | text without a Unicode mapping was turned into vector outlines (`--outline-fonts`) |
| `FONT_PAGE_RASTERIZED` | a page whose font could be neither embedded nor substituted was turned into an image, with the text kept invisible |
| `ANNOT_REMOVED` | annotations whose appearance uses a font that cannot be embedded were removed |
| `OCR_TEXT_LAYER_ADDED` | an invisible text layer from OCR was added (`--ocr`) |

## Colour and output intents

| Code | Meaning |
|---|---|
| `OUTPUT_INTENT_PRESENT` | the existing output intent was kept (routine) |
| `OUTPUT_INTENT_ADDED` | an output intent was added (sRGB, or the CMYK press profile) |
| `OUTPUT_INTENT_ADOPTED` | the document's own output intent was used |
| `OUTPUT_INTENT_REPLACED` | the existing output intent did not cover the colours used, so it was replaced |
| `OUTPUT_INTENTS_PRUNED` | several output intents were reduced to the single print intent |
| `OUTPUT_INTENT_TEXT_FIXED` | descriptive output intent entries that were not plain text were repaired |
| `OUTPUT_INTENT_PROFILE_REF_REMOVED` | an output intent referred to an external profile, which this level does not allow |
| `OUTPUT_INTENT_REF_INCOMPLETE` | an external press profile reference lacks required details |
| `OUTPUT_INTENT_EXTERNAL_ADDED` | an output intent referring to an external press profile was added (`x5n` with `--profile-url`) |
| `OUTPUT_INTENT_EXTERNAL_KEPT` | an output intent referring to an external press profile was kept |
| `OUTPUT_INTENT_EXTERNAL_MISSING` | the flavour needs an external press profile reference and none was found |
| `OUTPUT_INTENT_NOT_EXTERNAL` | the flavour needs an external reference, but the profile is only embedded |
| `OUTPUT_INTENT_EMBEDDED_TOO` | the flavour needs an external reference only, but the profile is embedded as well |
| `PAGE_OUTPUT_INTENTS_SET` | the output intent was also written on individual pages |
| `PAGE_OUTPUT_INTENTS_REMOVED` | page-level output intents were removed |
| `PAGE_OUTPUT_INTENT_INVALID` | a page-level print output intent has no usable profile |
| `DEST_PROFILE_IGNORED` | the `--dest-profile` colour space does not fit the target, so the built-in profile was used |
| `DEFAULT_PROFILE_USED` | a `--default-rgb`, `--default-cmyk` or `--default-gray` profile was used |
| `DEFAULT_RGB_INJECTED` | uncalibrated RGB was tied to sRGB |
| `DEFAULT_CMYK_INJECTED` | uncalibrated CMYK was tied to a calibrated CMYK profile |
| `DEVICE_COLOR_UNIFIED` | PDF/A-1 with both RGB and CMYK: RGB was converted to CMYK so one output intent covers it (visual difference possible) |
| `DEVICE_COLOR_OUTSIDE_INTENT` | CMYK or gray colour is used but the output intent describes an RGB or gray condition |
| `RGB_NOT_PERMITTED` | RGB colour is used, which a multi-colour press condition cannot print |
| `PROCESS_COLORANT_MISSING` | device colour is used but the press profile lacks that process ink |
| `ICC_PROFILE_REPLACED` | ICC profiles of a kind the level does not accept were replaced |
| `ICC_PROFILE_DOWNGRADED` | ICC profiles of a version the level does not accept were replaced |
| `ICC_COMPONENTS_SET` | the component count of an ICC colour space was corrected |
| `ICC_INTENT_DEDUPED` | colour spaces identical to the output intent were simplified to plain CMYK |
| `ICC_PROFILE_MISSING` | a colour profile named for conversion was not found; the conversion is refused |
| `GROUP_SPACE_CALIBRATED` | transparency groups were given a calibrated blending colour space |
| `SHADING_RGB_TAGGED`, `SHADING_CMYK_TAGGED` | uncalibrated shadings were tagged with the matching profile |
| `SEPARATION_UNIFIED` | spot colours sharing a name were given one alternate colour |
| `SEPARATION_ALTERNATE_FIXED` | the RGB alternate of a spot colour was converted |
| `COLORANT_RENAMED` | spot colour names that were not valid UTF-8 were renamed |
| `COLORANTS_COMPLETED` | missing spot colour entries of a multi-ink colour space were added |
| `DEVICEN_ATTRIBUTES_FIXED` | entries of a multi-ink colour space that the level does not allow were removed |
| `DEVICEN_REDUCED` | multi-ink colour spaces with too many inks were replaced by calibrated CMYK (visual difference possible) |
| `DEVICEN_MALFORMED` | a damaged multi-ink colour space was left as it was |
| `OVERPRINT_MODE_RESET` | the overprint mode was reset where calibrated CMYK is used |
| `TRANSFER_FUNCTION_REMOVED` | transfer curves that PDF/X-1a and PDF/X-3 do not allow were removed |
| `ANNOT_COLOR_REMOVED` | annotation colours were removed because the output intent is not RGB |
| `COLOR_CONVERTED` | colour was converted (`--convert-colors` or a profile) |
| `COLOR_CONVERSION_INCOMPLETE` | some objects kept their original colour; the detail says why |
| `COLOR_CONVERSION_SKIPPED` | the colour conversion did not run; the detail says why |
| `X1A_COLOR_CONVERTED` | RGB content was converted to CMYK for PDF/X-1a |
| `X1A_FUNCTION_APPROXIMATED` | calculator functions in shadings or tints were approximated during conversion |
| `X1A_COLOR_UNCONVERTIBLE` | colour that PDF/X-1a does not allow could not be converted |

## Transparency and rasterizing

| Code | Meaning |
|---|---|
| `TRANSPARENCY_FLATTENED` | a JPEG 2000 alpha channel was composited over white |
| `TRANSPARENCY_FLATTENED_VECTOR` | transparency was flattened into plain vector objects |
| `TRANSPARENCY_REGION_FLATTENED` | transparency was flattened in a small area, keeping the rest of the page as vectors |
| `TRANSPARENCY_REGIONS_RASTERIZED` | transparent areas were turned into images, each clipped to its object |
| `TRANSPARENCY_RASTERIZED` | whole pages with transparency were turned into images |
| `TRANSPARENCY_GROUP_REMOVED` | a transparency group was removed from a form |
| `PAGE_GROUP_MISSING` | a page uses transparency but defines no blending colour space |
| `BLEND_MODE_FIXED` | a non-standard blend mode was replaced |
| `ALPHA_CLAMPED` | opacity values out of range, or not numbers, were set to 1 |
| `SMASK_OPAQUE_REMOVED` | a fully opaque soft mask was removed (no visual change) |
| `SMASK_TO_STENCIL` | a two-level soft mask became a stencil mask (lossless) |
| `PAGES_RASTERIZED` | pages were turned into images (`--rasterize-pages`); the detail says whether the text was kept as invisible text |
| `VERSION_TRANSPARENCY_FLATTENED` | transparency was flattened because `--pdf-version` is below 1.4 |

Any of these with `(visual risk)` in the detail needed `--allow-visual-risk`.

## Images

| Code | Meaning |
|---|---|
| `IMAGE_INPUT_WRAPPED` | a JPEG input was wrapped as a one-page PDF |
| `IMAGE_DOWNSAMPLED` | images above `--image-max-ppi` were downsampled |
| `IMAGE_UPSAMPLED` | images were upsampled to a minimum resolution (a profile fix) |
| `IMAGE_RECOMPRESSED` | a lossy image was re-encoded as CMYK JPEG during colour conversion |
| `IMAGE_DEPTH_REDUCED` | a 16-bit image was reduced to 8 bits, as PDF 1.4 requires |
| `IMAGE_DEPTH_NORMALIZED` | an image with an unsupported bit depth was re-encoded |
| `IMAGE_COLORSPACE_RETAGGED` | a JPEG's colour space was corrected to match its data |
| `IMAGE_SIZE_CORRECTED` | a JPEG's declared size was corrected to match its data |
| `IMAGE_MASK_DROPPED` | a colour-key mask that no longer applied after CMYK conversion was removed |
| `IMAGE_KEPT_UNDECODABLE` | an image could not be decoded and was kept unchanged |
| `JPEG_DATA_REPAIRED` | a damaged JPEG was re-encoded from what can be recovered |
| `JPX_TRANSCODED` | a JPEG 2000 image was re-encoded losslessly |
| `JPX_NONCONFORMING` | a JPEG 2000 image breaks the archive rules and cannot be re-encoded |
| `JPX_IMAGE_NOTED` | a JPEG 2000 image was found; PDF/A-1 sets no rule on it (information) |
| `JBIG2_REENCODED` | a JBIG2 image was re-encoded losslessly for print exchange |
| `INLINE_IMAGE_FILTER_FIXED` | an inline image with a compression the level does not allow was re-encoded |
| `INTERPOLATE_REMOVED` | image smoothing was switched off, as the standard requires |
| `ALTERNATES_REMOVED` | alternate versions of an image were removed |
| `OPI_REMOVED` | an image replacement link for prepress systems was removed |
| `VERSION_IMAGES_REDUCED` | 16-bit images were reduced for `--pdf-version` below 1.5 |
| `VERSION_JPX_RECODED` | JPEG 2000 images were re-encoded as JPEG for `--pdf-version` below 1.5 |

## Page content and file syntax

| Code | Meaning |
|---|---|
| `CONTENT_FILTERED` | page content was normalized to the standard's limits (routine) |
| `CONTENT_NONCONFORMING` | page content breaks syntax rules or size limits |
| `CONTENT_COLOR_OPERANDS_INVALID` | colour operators with the wrong number of values |
| `CONTENT_UNDECODABLE` | a page content stream could not be decoded and was left as it was |
| `CONTENT_MISSING_REPLACED` | page content stored with an image compression cannot be drawn and was replaced with empty content |
| `DANGLING_REFS_REMOVED` | drawing operators that named resources missing from the page were removed |
| `RESOURCES_COMPLETED` | resources used by page content were listed explicitly |
| `RESOURCES_PRUNED` | unused resources were removed to stay under the PDF/A-1 limit |
| `RESOURCE_CYCLE_CUT` | a form or pattern that draws itself was broken out of the loop |
| `FORM_BBOX_ADDED` | a form was given its required bounding box |
| `FORM_RESOURCES_ADDED` | a form was given its own resources |
| `PATTERN_PLACEHOLDER_ADDED`, `XOBJECT_PLACEHOLDER_ADDED` | content named a pattern or form that exists nowhere; an empty one was added |
| `TILING_PATTERN_COMPLETED` | missing entries of a tiling pattern were filled in |
| `SHADING_BBOX_REMOVED` | a damaged bounding box was removed from a shading |
| `POSTSCRIPT_XOBJECT_REMOVED` | an embedded PostScript fragment was removed |
| `RENDERING_INTENT_FIXED` | an invalid rendering intent was removed |
| `HALFTONE_REMOVED`, `HALFTONE_NAME_REMOVED`, `HALFTONE_ORIGIN_REMOVED`, `HALFTONE_PHASE_REMOVED` | halftone settings the level does not allow were removed |
| `HALFTONE_TF_ADDED`, `HALFTONE_TF_REMOVED` | a halftone transfer curve was added or removed, as the level requires |
| `SEPARATION_INFO_REMOVED` | separation information was removed from a page (a print exchange page is a composite) |
| `USER_UNIT_APPLIED` | a page scale factor was applied to the page, as PDF 1.4 has none |
| `LIMITS_CLAMPED` | values beyond PDF 1.4 limits were clamped |
| `PAGE_BOX_CLAMPED` | page boxes were brought into the allowed size range |
| `PAGE_BOXES_NORMALIZED` | a page had no usable media box and one was set |
| `TRIMBOX_ADDED` | a trim box was added to pages that lacked one |
| `PAGE_TREE_REBALANCED` | the page list was split into nested nodes to stay under a size limit |
| `PAGE_TREE_REBUILT` | a damaged page list was rebuilt |
| `FILE_STRUCTURE_NONCONFORMING` | the file's cross-reference table was damaged and had to be repaired |
| `STREAM_FILTER_REPLACED` | streams with a compression the level does not allow were decoded |
| `LZW_REENCODED` | LZW compression was replaced |
| `NONSTANDARD_FILTER_REMOVED` | streams with an unknown compression were emptied (content not recoverable) |
| `STREAM_EXTERNAL_REFS_REMOVED` | streams that pointed at external files were cut loose |
| `THUMBNAIL_REMOVED` | a page thumbnail with a compression the level does not allow was removed |
| `ENCRYPTION_REMOVED` | the document was encrypted; encryption was removed |
| `CATALOG_KEY_REMOVED` | an entry the level does not allow was removed from the document catalog |
| `VERSION_LAYERS_FLATTENED` | layers were merged into the page for `--pdf-version` below 1.5 |
| `SCAN_INCOMPLETE` | part of the document could not be read; findings may be incomplete (warning) |

## Annotations, forms, actions and layers

| Code | Meaning |
|---|---|
| `ANNOTATION_REMOVED` | an annotation type the level forbids was removed |
| `ANNOTATION_DRAWN_INTO_PAGE` | an annotation's visible appearance was drawn into the page |
| `ANNOTATION_APPEARANCE_BAKED` | annotation appearances were drawn into the page content |
| `ANNOT_FLAGS_FIXED` | annotation flags were set as the standard requires |
| `APPEARANCE_ADDED` | an annotation without an appearance was given an empty one |
| `APPEARANCE_EXTRA_REMOVED` | rollover and down appearances were removed, keeping the normal one |
| `APPEARANCE_FLATTENED` | an appearance with several states was reduced to one |
| `APPEARANCE_STATE_ADDED`, `APPEARANCE_STATES_WRAPPED` | a missing appearance state was added to a widget |
| `TRAPNET_FONT_SUBSTITUTION_REMOVED` | a font substitution list was removed from a trap network annotation |
| `ACROFORM_REMOVED` | the interactive form was removed (not usable in PDF/X) |
| `NEED_APPEARANCES_REMOVED` | the "regenerate appearances" form flag was removed |
| `XFA_REMOVED` | XFA form data was removed |
| `SIGNATURE_FIELDS_REMOVED` | unsigned signature fields were removed (PDF/E-1) |
| `ACTION_REMOVED` | a forbidden action (script, launch, open action and so on) was removed |
| `ACTION_CHAIN_REMOVED` | a chain of follow-on actions was removed |
| `ADDITIONAL_ACTIONS_REMOVED` | event-triggered actions were removed |
| `JAVASCRIPT_REMOVED` | document-level JavaScript was removed |
| `DESTS_REMOVED` | the named destination list was removed |
| `ALTERNATE_PRESENTATIONS_REMOVED` | slideshow presentations were removed |
| `PRESENTATION_STEPS_REMOVED` | page presentation steps were removed |
| `MODEL_RESOURCES_REMOVED` | a resource list was removed from a 3D stream |
| `OPTIONAL_CONTENT_REMOVED` | layer membership was removed from an annotation |
| `OCG_LISTED` | layers missing from the document's layer list were added |
| `OCG_NAME_ADDED` | a layer without a name was given one |
| `OC_DEFAULT_CONFIG_ADDED` | the required default layer configuration was added |
| `OC_CONFIG_NAME_ADDED`, `OC_CONFIG_NAME_DEDUPED` | a layer configuration was given a name, or a unique one |
| `OC_ORDER_COMPLETED` | layers missing from the layer panel order were added |
| `OC_AS_REMOVED` | automatic layer state changes were removed |

## Structure and accessibility

| Code | Meaning |
|---|---|
| `MARKINFO_SET` | the document was marked as tagged |
| `STRUCT_TREE_SYNTHESIZED` | a minimal structure tree was generated; tagging quality is limited (warning) |
| `STRUCT_TREE_REBUILT` | the existing structure tree was replaced by a generated one; the detail says why |
| `UA_STRUCTURE_BUILT` | pages were tagged as paragraphs, figures and annotations for PDF/UA |
| `UA_STRUCTURE_REPAIRED` | the existing structure tree was kept and repaired for PDF/UA |
| `UA_STRUCT_DEST`, `UA_STRUCT_DEST_KEPT` | links were pointed at structure elements, as PDF/UA-2 requires, or kept |
| `ROLEMAP_NORMALIZED` | the structure role map was cleaned up |
| `STRUCT_NAMES_SANITIZED` | structure type names that were not valid UTF-8 were replaced |
| `STRUCT_IDS_REPAIRED` | structure element IDs were repaired |
| `ARTIFACT_MARKED` | untagged content was marked as decoration so assistive technology skips it |
| `HEADINGS_MERGED` | adjacent headings of the same level were merged |
| `LIST_NUMBERING_SET` | the numbering style of lists was set |
| `UA_<checkpoint>` | from `--check --level ua1` or `ua2`: a PDF/UA failure, named by its checkpoint number, such as `UA_01-003` for PDF/UA-1 or `UA_7.18.1-1` for PDF/UA-2 |

## Attachments and e-invoices

| Code | Meaning |
|---|---|
| `FACTURX_ATTACHED` | the invoice XML was embedded, with its standard and profile |
| `EINVOICE_PROFILE_LIMITED` | the invoice profile is valid but does not carry the full invoice, so some mandates do not accept it |
| `EINVOICE_PDFA4_CONTAINER` | the invoice was put in PDF/A-4f; many recipients still expect PDF/A-3 |
| `SOURCE_EMBEDDED` | the original document was attached (`--embed-source`) |
| `AF_ARRAY_ADDED` | attachments were linked to the document as associated files |
| `AF_RELATIONSHIP_ADDED` | an attachment was given its relationship to the document |
| `MARKED_CONTENT_AF_MOVED` | associated files were moved from a content marker to the page |
| `ASSOCIATED_FILES_INVALID` | associated-file entries are not lists of file specifications |
| `ASSOCIATED_FILES_REMOVED` | associated-file references were removed |
| `EMBEDDED_FILES_REMOVED`, `EMBEDDED_FILE_REMOVED` | attachments were removed at a level that does not allow them |
| `EMBEDDED_FILES_TREE_ADDED` | PDF/A-4f needs an attachment list; one was added |
| `EMBEDDED_FILES_ENROLLED` | attachments missing from the attachment list were added to it |
| `EMBEDDED_FILE_KEYS_MISSING` | an attachment lacks its file name entries |
| `EMBEDDED_FILE_MIME_ADDED` | an attachment was given a default file type |
| `REFERENCE_XOBJECT_FIXED` | a reference to an external page without a usable file was removed |
| `REFERENCE_XOBJECT_INCOMPLETE` | a reference to an external page lacks the file identifiers |
| `REFERENCE_XOBJECT_KEPT` | a reference to an external page was kept (allowed at this level) |

## Print standards and variable data

| Code | Meaning |
|---|---|
| `VT_DPART_BUILT` | the PDF/VT record structure was built from `--vt-records` |
| `VT_SINGLE_PART` | no records were given, so the whole document became one record |
| `VT_DPART_PRESENT` | the existing record structure was kept |
| `VT_DPART_REBUILT` | the record structure was replaced; the detail says why |
| `VT_RECORDS_INVALID` | `--vt-records` did not form contiguous page ranges covering the document |
| `VT_PART_METADATA_FIXED` | invalid entries in record metadata were removed |
| `VT_REUSE_HINT_FIXED` | reuse hints that did not match how objects are used were removed |
| `VT_ENCAPSULATION_DROPPED` | the self-contained flag was removed from forms that depend on their surroundings |
| `VT_IMAGE_INTENT_ADDED` | self-contained images were given a rendering intent |

## Signing, certificates and process

| Code | Meaning |
|---|---|
| `DOCUMENT_SIGNED` | the output was signed (`--sign`) |
| `SIGNATURE_REMOVED` | existing signatures were removed; conversion invalidates them, so sign the result again |
| `SIGNATURE_INVALIDATED` | the input was signed; only the new signature will verify |
| `CERTIFIED` | a check certificate was embedded (`--certify`) |
| `CERTIFY_FAILED` | the check certificate could not be embedded |
| `FALLBACK_RECONVERTED` | the first result was not compliant, so it was converted a second time (`--reconvert`) |
| `FALLBACK_RASTERIZED` | the regular conversion did not reach compliance, so pages were turned into images (`--fallback-raster`, `--fallback-pages`) |

## Analysis

With `--analyze`, the census arrives under `analysis`, all at severity 1:

| Code | Meaning |
|---|---|
| `ANALYZE_HAIRLINE` | strokes thinner than 0.125 pt |
| `ANALYZE_SMALL_TEXT` | text below 4 pt |
| `ANALYZE_RICH_BLACK` | objects in rich black (over 90% K plus other inks) |
| `ANALYZE_INVISIBLE_TEXT` | invisible text that is not used for clipping |
| `ANALYZE_IMAGE_LOWRES` | images below 250 ppi (550 ppi for one-bit images) at their placed size |
| `ANALYZE_IMAGE_HIGHRES` | images above 450 ppi (3600 ppi for one-bit images) |
| `ANALYZE_IMAGE_FORMATS` | the image compressions in use |
| `ANALYZE_TRANSPARENCY` | uses of transparency |
| `ANALYZE_OVERPRINT` | graphics states that switch on overprint |
| `ANALYZE_COLOR_USAGE` | the colour spaces in use |
| `ANALYZE_FONT_NOT_EMBEDDED` | fonts that are not embedded |
| `ANALYZE_FONT_NO_TOUNICODE` | fonts without a Unicode mapping |
| `ANALYZE_TYPE3_FONTS` | Type 3 fonts in use |
| `ANALYZE_MIXED_PAGE_SIZES` | pages of different sizes |
| `ANALYZE_ANNOTATIONS` | the annotations in use |
| `ANALYZE_OUTPUT_INTENT` | whether an output intent is present |

## Profiles and plans

These arrive under `analysis` when a [preflight profile](/preflight) runs:

| Code | Meaning |
|---|---|
| `CHECK_HIT` | a profile check found something |
| `CHECK_NOT_EVALUATED` | a condition could not be evaluated for this file |
| `PROFILE_INVALID` | the profile is not valid; the detail names the field |
| `PROFILE_EMPTY` | check mode was asked for, but the profile has no checks |
| `PROFILE_TARGET` | the profile's `convert` section chose the target |
| `PROFILE_ICC_MISSING` | a colour profile named in the `convert` section was not found |
| `VARIABLE_SET` | a profile variable received a value |
| `FIX_SCHEDULED`, `FIX_DONE`, `FIX_NOT_NEEDED`, `FIX_UNKNOWN` | a profile fix was planned, changed the file, had nothing to do, or is not in the catalogue |
| `PLAN_STEP`, `PLAN_STEP_FAILED`, `PLAN_NO_CONVERSION` | plan progress |
