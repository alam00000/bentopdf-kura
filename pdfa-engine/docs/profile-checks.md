# Kura profile checks and fixes

Generated from `core/registry/catalogue.json` by `scripts/gen_registry.py`. Do not edit by hand.

A condition names a check, an operator and a value, plus any named parameters:

```json
{"check": "page.box_size", "box": "trim", "dimension": "width", "op": ">=", "value": "210mm"}
```

Parameters left out take the default shown below. Unknown checks, parameters, operators or
enumeration values make the profile invalid (output code `PROFILE_INVALID`).

## Operators

| Operator | Meaning |
|---|---|
| `<` | Less than (numbers, versions, dates). |
| `<=` | Less than or equal. |
| `==` | Equal. Text is compared exactly, numbers within the atom's tolerance. |
| `!=` | Not equal. |
| `>=` | Greater than or equal. |
| `>` | Greater than. |
| `multiple_of` | The number is a whole multiple of the value (within the tolerance). |
| `is` | Boolean test: the check's answer is the given true/false value. A false test matches exactly the objects for which the property is false. |
| `in` | The value equals one entry of the given list. |
| `not_in` | The value equals no entry of the given list. |
| `contains` | The value contains the given text. |
| `starts_with` | The value starts with the given text. |
| `ends_with` | The value ends with the given text. |
| `matches` | The whole value matches the regular expression. |
| `finds` | The regular expression matches somewhere in the value. |
| `occurs_in` | The value appears inside the given text. |
| `includes` | The list contains the given entry (or all given entries). |
| `excludes` | The list contains none of the given entries. |
| `count` | Compare the number of list entries; combine with a comparison: {"op": "count", "compare": ">", "value": 2}. |
| `fits_within` | The measured size is not larger than the given size in either dimension. |
| `exceeds` | The measured size is larger than the given size in at least one dimension. |

Every condition also accepts:

- `negate`: Inverts any operator.
- `case_sensitive`: false makes text operators ignore case (default true).
- `tolerance`: Numeric tolerance in the value's unit.

## Value types

| Type | Meaning |
|---|---|
| `bool` | true or false. Test with operator 'is' and value true or false. |
| `number` | A plain number. |
| `count` | A whole number of items. |
| `length` | A length; values may carry a unit (pt, mm, cm, in). Without a unit the check's unit parameter applies. |
| `percent` | A percentage 0-100 (ink amounts may exceed 100 for totals). |
| `angle` | Degrees. |
| `resolution` | Pixels per inch; values may be written as 300 or '300ppi'. |
| `ratio` | A ratio such as 1.414. |
| `bytes` | A size in the unit given by the check's unit parameter. |
| `version` | A PDF version such as 1.7 or 2.0, compared numerically. |
| `date` | An ISO 8601 date, compared chronologically. |
| `text` | A string. |
| `enum` | One of the values listed in the check's notes. |
| `list` | A set of values. 'includes' and 'excludes' test membership; 'count' compares the number of entries. |
| `size` | Width and height given in the check's parameters; compared in either orientation if allowed. |

## Checks

### Document

The file as a whole: version, pages, viewing settings, document-level features.

#### `document.binding`: Reading direction / binding side

Binding side declared in the viewer preferences (Direction).

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 12.2
- Notes: Values: left, right.

No parameters.

#### `document.colour_spaces`: Colour spaces used

List of colour space families used anywhere in the document or on the page.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 8.6

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `document` | document, page |  |

#### `document.content_stream_size`: Largest page content size

Size of the largest page content stream, a sign of overly complex pages.

- Value: `bytes`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `unit` | enum | `kb` | bytes, kb, mb |  |

#### `document.created_by_print_driver`: Created by a print-to-PDF driver

The producer string shows the file was made by a printer driver, which typically loses structure and fonts.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: Kura

No parameters.

#### `document.damaged`: File is damaged

The file could only be opened after repairing its cross-reference information.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.5.4

No parameters.

#### `document.data_after_eof`: Data after end of file

Bytes other than an end-of-line follow the last %%EOF marker.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-2 6.1.3; ISO 32000-2 7.5.5

No parameters.

#### `document.extension_level`: Developer extension level

Extension level declared in the catalog's Extensions dictionary for a registered prefix.

- Value: `number`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 7.12

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `prefix` | text | empty |  | Leave empty to consider every declared prefix. |

#### `document.extension_version_mismatch`: Extension base version mismatch

An extension declares a BaseVersion different from the version the catalog declares.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.12.2

No parameters.

#### `document.file_size`: File size

Size of the PDF file on disk.

- Value: `bytes`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `unit` | enum | `kb` | bytes, kb, mb | Kilobytes read naturally for typical document sizes. |

#### `document.form_nesting`: Form XObject nesting depth

Deepest nesting of form XObjects in the document.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 8.10

No parameters.

#### `document.has`: Document contains a feature

The document contains at least one instance of the selected feature.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.7.2
- Notes: One check for every 'document has X' question; each feature is documented in the reference table.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `feature` | enum | required | bookmarks, page_labels, thumbnails, article_threads, named_destinations, unused_destinations, form_fields, interactive_annotations, file_specifications, alternate_presentations, piece_info, xmp_metadata, non_plain_text_xmp, named_optional_content, optional_content, mark_info, needs_rendering, javascript, embedded_files_name_tree |  |

#### `document.header_conforms`: File header conforms to a standard

The %PDF- header line and the binary comment that follows it meet the rules of the selected standard.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.5.2; ISO 19005-1 6.1.2; ISO 19005-2 6.1.2; ISO 24517-1 6.1

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `standard` | enum | `pdfa-2b` | pdf-2.0, pdfa-1b, pdfa-2b, pdfe-1 | PDF/A-2 is the most common archive target. |

#### `document.id`: File identifier

State of the ID array in the trailer.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 14.4; ISO 19005-2 6.1.3
- Notes: Values: missing, present, malformed, inconsistent (the two halves of the ID differ between incremental updates in a way the standard forbids), not_in_last_trailer, indirect.

No parameters.

#### `document.job_options`: Embedded creation settings

The file carries the PDF creation settings (job options) that were used to make it.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `compare_to` | text | empty |  | Text of a settings file to compare with. Empty: only presence is tested. |

#### `document.language`: Document language

Natural language declared by the catalog's Lang entry.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 14.9.2; ISO 14289-1 7.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `test` | enum | `value` | value, present, valid_tag, is_text_string | The value itself is the usual subject. |

#### `document.largest_stream`: Largest stream

Size of the largest stream in the file.

- Value: `bytes`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `unit` | enum | `kb` | bytes, kb, mb |  |

#### `document.linearized`: Linearized for fast web view

The file carries linearization hints so viewers can show the first page before the whole file arrives.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 Annex F

No parameters.

#### `document.mark_info_extra_keys`: MarkInfo has other entries

MarkInfo contains entries besides Marked (Suspects, UserProperties).

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 14.7.1

No parameters.

#### `document.marked`: Marked as tagged

The MarkInfo dictionary declares the document as tagged (Marked true).

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 14.7.1; ISO 14289-1 7.1

No parameters.

#### `document.needs_rendering`: NeedsRendering is true

The catalog's NeedsRendering flag asks viewers to render an XFA form instead of the PDF pages.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.7.2; ISO 19005-2 6.4.2

No parameters.

#### `document.open_view`: Initial view settings

How the document asks to be displayed when opened.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 7.7.2; ISO 32000-2 12.2; ISO 14289-1 7.1
- Notes: Presence checks use the operator != with an empty value.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `setting` | enum | `page_layout` | page_layout, page_mode, open_page, zoom, show_title, viewer_preferences, print_scaling, clip_to | Page layout is the most commonly prescribed setting. |

#### `document.page_count`: Number of pages

How many pages the document has.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 7.7.3

No parameters.

#### `document.pages_uniform`: All pages alike

True when every page shares one size, one orientation, or both, allowing a small tolerance.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `aspect` | enum | `size_and_orientation` | size, orientation, size_and_orientation |  |
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `tolerance` | length | `1pt` |  | One point absorbs rounding in typical box values. |

#### `document.pdf_version`: PDF version

The PDF version the file declares, read from the file header or from the catalog's Version entry.

- Value: `version`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 7.5.2; ISO 32000-2 7.7.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `source` | enum | `effective` | effective, header, catalog | The effective version is the higher of header and catalog, as ISO 32000-2 7.2.2 prescribes. |

#### `document.portfolio`: Portfolio (collection)

The catalog contains a Collection dictionary, so viewers present the embedded files as a portfolio.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 12.3.5

No parameters.

#### `document.requirements`: Declares viewer requirements

The catalog has a Requirements entry that names features a viewer must support.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 12.11

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `pdf2_only` | bool | `false` |  | Report only requirement types introduced by PDF 2.0. Off by default so any declaration is reported. |

#### `document.script`: Custom script test

Runs a JavaScript expression against the document facts (window.kura.document) and reports when it returns true.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: Kura
- Notes: Runs in the sandboxed script engine with a 5 s limit.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `script` | text | required |  |  |

#### `document.stream_compression`: Stream compression

Compression used by streams in the file.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.4; ISO 19005-1 6.1.10
- Notes: Values are filter names without the slash (FlateDecode, LZWDecode, ...) plus 'none' for an uncompressed stream.

No parameters.

#### `document.stream_layout`: Stream dictionary problems

Problems in how streams are written: length, line ends, filters and external files.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.3.8; ISO 19005-2 6.1.7
- Notes: Values: wrong_length, keyword_line_ends, external_file, external_filter, external_decode_parms, has_dl_entry, dl_inaccurate, uses_crypt_filter.

No parameters.

#### `document.title`: Document title

The document has a title and asks viewers to show it instead of the file name.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 14289-1 7.1; WCAG 2.2 2.4.2; Matterhorn 06-003; Matterhorn 07-001; Matterhorn 07-002
- Notes: Values: missing (no title in the XMP metadata or the information dictionary), not_in_metadata (no dc:title in the XMP metadata), not_displayed (ViewerPreferences DisplayDocTitle is not true).

No parameters.

#### `document.trailer`: Trailer entries

Required trailer entries are present and of the right type.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.5.5
- Notes: Value lists the problems found: missing_size, size_not_integer, missing_root, has_info, encrypted.

No parameters.

#### `document.uses_object_streams`: Uses compressed object streams

Objects are stored inside compressed object streams.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.5.7

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `except_structure` | bool | `false` |  | Ignore object streams that only hold structure-tree objects. Off: any use counts. |

#### `document.version_conflict`: Header and catalog versions differ

The catalog declares a newer PDF version than the file header. Allowed by ISO 32000, but some tools read only the header.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.7.2

No parameters.

### File syntax

How the file is written: objects, streams, cross-references, content-stream operators, limits.

#### `syntax.broken_reference`: Reference to a missing object

An object reference, or a reference XObject's target file, cannot be resolved.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 7.3.10; ISO 32000-2 8.10.4
- Notes: Values: object, external_file.

No parameters.

#### `syntax.content_stream_error`: Content stream violates the specification

An operator in a page or form content stream is used incorrectly.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.8.2; ISO 32000-2 Annex A; ISO 19005-2 6.2.2
- Notes: Values: unknown_operator, wrong_operand_type, wrong_operand_count, operator_out_of_place, stream_state, unbalanced_operator, unmatched_operator, unbalanced_compatibility, unknown_operand_value, unused_operands, operator_takes_no_operands, missing_resource, missing_property_list, unknown_colour_space_family, corrupt_inline_dictionary, resources_unreadable, nested_marked_content.

No parameters.

#### `syntax.dictionary_error`: Dictionary or array violates the specification

An entry in a dictionary or array is missing, unknown, of the wrong type, or has a value the specification does not allow.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.3; ISO 19005-2 6.1
- Notes: Values: required_key_missing, dependent_key_missing, unknown_key, key_newer_than_version, wrong_type, unexpected_value, must_be_indirect, must_be_absent, exclusive_keys, array_wrong_size, array_element_type, array_unexpected_value, array_element_must_be_indirect, filter_missing, filter_params_missing, multiple_params_required.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `object_type` | text | empty |  | Limit to one dictionary Type, e.g. Page or Annot. Empty: all object types. |

#### `syntax.limit`: Implementation limit

Extreme values used anywhere in the file, for comparison with the implementation limits of a standard.

- Value: `number`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 Annex C; ISO 19005-2 6.1.13

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `measure` | enum | required | largest_integer, smallest_integer, largest_real, smallest_real, real_closest_to_zero_positive, real_closest_to_zero_negative, longest_string, longest_content_string, longest_name, largest_array, largest_dictionary, deepest_graphics_state_nesting |  |

#### `syntax.linearization_id_mismatch`: Linearized file has two different IDs

The first-page trailer and the main trailer of a linearized file carry different IDs.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-1 6.1.3

No parameters.

#### `syntax.object_count`: Number of indirect objects

Count of indirect objects in the file.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 Annex C

No parameters.

#### `syntax.object_layout`: Indirect object layout

Problems in how indirect objects are written: obj/endobj keywords, line ends and numbering.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.3.10; ISO 19005-2 6.1.9
- Notes: Values: no_eol_before_obj, no_eol_after_obj, no_eol_before_endobj, no_eol_after_endobj, bad_object_number, bad_obj_keyword_spacing.

No parameters.

#### `syntax.postscript`: Embedded PostScript

The document carries PostScript code: PS operators, PostScript XObjects or forms with a PS subtype.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 8.8.2; ISO 19005-2 6.2.9
- Notes: Values: operator, xobject, form.

No parameters.

#### `syntax.resource_error`: Damaged embedded resource

A font, image, ICC profile or XMP packet could not be read.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.3
- Notes: Values: font, image, icc_profile, xmp.

No parameters.

#### `syntax.string_encoding`: String and name encoding

Encoding problems in strings and names.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.3.4; ISO 32000-2 7.3.5; ISO 19005-2 6.1.6
- Notes: Values: hex_invalid_character, hex_odd_digits, name_not_utf8.

No parameters.

#### `syntax.unknown_operator`: Operator unknown to a PDF version

Content uses an operator that the selected PDF version does not define.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 Annex A; ISO 15930-1 6.1

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `version` | enum | `1.3` | 1.0, 1.1, 1.2, 1.3 | PDF 1.3 is the base of PDF/X-1a and PDF/X-3. |

#### `syntax.xref_layout`: Cross-reference section layout

Problems in classic cross-reference subsection headers.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.5.4; ISO 19005-1 6.1.4
- Notes: Values: bad_subsection_header, subsection_not_eol_separated.

No parameters.

### Security

Encryption and permissions.

#### `security.encryption`: Encryption

Whether and how the document is encrypted.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 7.6; ISO 19005-2 6.1.3
- Notes: Values: none, rc4, aes128, aes256, other.

No parameters.

#### `security.encryption_dictionary`: Encryption dictionary details

Properties of the encryption dictionary.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 7.6.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `entry` | enum | `version` | version, has_version, has_permissions, direct_strings, direct_arrays, direct_dictionaries, crypt_filter_direct_strings, crypt_filter_direct_arrays, crypt_filter_direct_dictionaries, crypt_filter_decode_name |  |

#### `security.permission`: Permission denied

The document's permissions forbid the selected action.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 7.6.4.2; ISO 14289-1 7.16

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `action` | enum | `copy_text` | print, modify, copy_text, annotate, fill_forms, accessibility, assemble, print_high | Text extraction matters most for accessibility (ISO 14289-1 7.16). |

### Page

Page geometry, boxes, labels, rotation and page-level properties.

#### `page.aspect_ratio`: Aspect ratio

Longer side divided by shorter side of a page box.

- Value: `ratio`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |

#### `page.box_offset`: Page box offset

Horizontal or vertical distance of a box's lower-left corner from the media box corner or from the origin.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `axis` | enum | `horizontal` | horizontal, vertical |  |
| `from` | enum | `media` | media, origin |  |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |

#### `page.box_present`: Page box present

The page (or an ancestor) defines the box explicitly.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 32000-2 14.11.2; ISO 15930-7 6.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |

#### `page.box_relation`: Relation between two page boxes

How one page box relates to another.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: page
- References: ISO 32000-2 14.11.2
- Notes: Values: same, encloses, inside, overlaps, apart, centered.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `crop` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `other` | enum | `media` | media, crop, bleed, trim, art |  |

#### `page.box_size`: Page box dimension

Width, height or area of one of the page boxes, with the page rotation and user unit applied unless 'raw' is set.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 32000-2 14.11.2; ISO 32000-2 7.7.3.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `dimension` | enum | `width` | width, height, longer, shorter |  |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |
| `raw` | bool | `false` |  | Use default user space without UserUnit scaling. Off: sizes are as the reader sees them. |

#### `page.boxes_nested`: Page boxes are properly nested

Art and trim lie within bleed, bleed within crop, crop within media.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 32000-2 14.11.2; ISO 15930-7 6.2

No parameters.

#### `page.content_stream`: Page content stream

Properties of the page's content stream.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: page
- References: ISO 32000-2 7.8.2
- Notes: compression lists filters or 'none'; ascii_encoded is ascii_hex, ascii85 or no.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `compression` | compression, size_kb, ascii_encoded |  |

#### `page.empty`: Page is empty

Nothing visible is drawn on the page, or inside the selected box.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `within` | enum | `page` | page, trim, bleed |  |

#### `page.has_measure_viewport`: Geospatial or measure viewport

The page has viewports with measurement dictionaries.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 32000-2 12.9

No parameters.

#### `page.has_no_text`: Page has no text

The page draws something but contains no text, typical of scans without OCR.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 14289-1 7.3

No parameters.

#### `page.in_range`: Page is in a range

The page falls in a page range such as '1-3, 7, even'.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `pages` | text | required |  |  |

#### `page.label`: Page label

The page label and its numbering style.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: page
- References: ISO 32000-2 12.4.2
- Notes: Styles: decimal, upper_roman, lower_roman, upper_letters, lower_letters, none.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `part` | enum | `label` | label, style, prefix |  |

#### `page.media_origin_not_zero`: Media box not at origin

The media box's lower-left corner is not 0 0.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 32000-2 14.11.2

No parameters.

#### `page.mostly_rotated_text`: Most text is rotated

Most of the page's text runs at an angle other than 0 degrees, a sign of a wrongly rotated scan.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: Kura

No parameters.

#### `page.number`: Page number

One-based position of the page in the document.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 32000-2 7.7.3

No parameters.

#### `page.orientation`: Orientation

Whether a page box is portrait or landscape.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: page
- References: Kura
- Notes: Values: portrait, landscape (a square box counts as portrait).

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `media` | media, crop, bleed, trim, art | The media box is the sheet the page is placed on. |

#### `page.output_intent`: Page-level output intent

The page has its own output intents and how they relate to the document's.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: page
- References: ISO 32000-2 14.11.5; ISO 15930-9 6
- Notes: Values: none, same_as_document, different.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `subtype` | enum | `pdfx` | pdfx, pdfa, any |  |

#### `page.parity`: Odd or even page

Tells odd-numbered pages from even-numbered ones.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: page
- References: Kura
- Notes: Values: odd, even.

No parameters.

#### `page.private_data`: Private application data

The page carries private application data (PieceInfo, separation info, private drawing-program data).

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: page
- References: ISO 32000-2 14.5
- Notes: Values: piece_info, separation_info, application_private.

No parameters.

#### `page.processing_steps_entry`: Page declares processing steps

The page has a processing-steps entry in its metadata or dictionary.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 19593-1 5

No parameters.

#### `page.resources_missing`: Resources missing

The page or a form uses named resources but has no Resources dictionary.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 32000-2 7.8.3

No parameters.

#### `page.rotation`: Page rotation

The Rotate value, normalised to 0, 90, 180 or 270.

- Value: `angle`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 32000-2 7.7.3.3

No parameters.

#### `page.single_image`: Page is one image

The page consists of a single image and nothing else, typical of scans.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: Kura

No parameters.

#### `page.size`: Page size

Compares the trim size (or another box) with a named or given size, in either orientation.

- Value: `size`
- Operators: `==`, `!=`, `fits_within`, `exceeds`
- Applies to: page
- References: ISO 216; ISO 32000-2 14.11.2
- Notes: Value: a paper name (A0-A10, B0-B10, C0-C10, Letter, Legal, Tabloid, Ledger) or width x height such as "210 x 297 mm"; or leave out the value and give width and height. Operators: == (same size), != , fits_within (not larger in either dimension), exceeds.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `width` | length | empty |  | Width of the size to compare with; use with height instead of a value. |
| `height` | length | empty |  | Height of the size to compare with; use with width instead of a value. |
| `tolerance` | length | `1pt` |  | One point absorbs rounding in typical box values. |
| `either_orientation` | bool | `true` |  | Portrait and landscape of the same size are the same paper size. |

#### `page.text_contains`: Page text contains

The extracted text of the page contains the given text or matches a pattern.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: page
- References: Kura

No parameters.

#### `page.transparency_group`: Page transparency group

Properties of the page's own transparency group.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: page
- References: ISO 32000-2 11.6.6; ISO 19005-2 6.2.10
- Notes: Values: present, knockout, has_colour_space, contains_transparency, not_needed (no transparent content).

No parameters.

#### `page.trim_or_art_only`: Exactly one of trim and art box

The page defines a trim box or an art box but not both, as PDF/X requires.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 15930-7 6.2

No parameters.

#### `page.unmappable_text`: Share of text without Unicode

Share of the page's characters, in percent, that have no Unicode meaning.

- Value: `percent`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 32000-2 9.10; ISO 14289-1 7.21.7

No parameters.

#### `page.user_unit`: Page scaling (user unit)

The UserUnit factor of the page; 1 means unscaled.

- Value: `number`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 32000-2 7.7.3.3

No parameters.

#### `page.uses_transparency`: Page uses transparency

Something on the page is transparent (soft mask, constant alpha below 1, or a non-Normal blend mode).

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 32000-2 11; ISO 15930-1 6.3

No parameters.

### Print production

Ink, bleed, separations, spot naming, halftones and trapping.

#### `print.bleed`: Bleed amount

Distance from the trim box to the bleed box (or media box when no bleed box exists) on one side or on the narrowest side.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 15930-7 6.2; Ghent Workgroup specification 2015
- Notes: Typical requirement: at least 3 mm.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `side` | enum | `smallest` | smallest, left, right, top, bottom | The narrowest side decides whether bleed is sufficient. |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |

#### `print.bleed_missing`: Content does not bleed

Objects touching the trim edge stop at the trim edge instead of extending into the bleed.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: Ghent Workgroup specification 2015

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `min` | length | `3mm` |  | 3 mm is the bleed most printers ask for (Ghent Workgroup and common print practice). |
| `shape` | enum | `trim` | trim, die_line | die_line: measure around a cutting contour drawn on the page. |
| `die_line_check` | text | empty |  | Name of a check that selects the cutting-contour objects. |

#### `print.empty_area`: Empty area on the page

The page has a blank region of at least the given size, for example to reserve space for a stamp.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `width` | length | required |  |  |
| `height` | length | required |  |  |
| `margin` | length | `0mm` |  |  |

#### `print.flatness`: Flatness tolerance

Flatness tolerance set in the graphics state.

- Value: `number`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 10.6.2

No parameters.

#### `print.halftone`: Halftone screen

Properties of the halftone in effect for an object, per colorant.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 10.6; ISO 19005-2 6.2.5
- Notes: Frequency in lines per inch, angle in degrees. Halftones in PDF/A must be type 1 or 5 without transfer functions.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `present` | present, type, frequency, angle, spot_function, accurate_screens, name, origin, phase |  |
| `colorant` | enum | `any` | any, cyan, magenta, yellow, black |  |

#### `print.images_on_cmy_only`: Images on colour plates only

Images on the page (or in the document) use cyan, magenta or yellow but no black, a sign of unintended RGB-to-CMYK conversion of grey images.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `page` | page, document |  |

#### `print.ink_area`: Area above an ink amount

Area of the page where ink reaches at least the given amount.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 12647-2 5.3
- Notes: The value is an area in square units of the chosen unit.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `ink` | percent | `300%` |  | The common sheet-fed coated limit (ISO 12647-2 / Ghent Workgroup). |
| `inks` | enum | `all` | all, process, black, spot |  |
| `sample_resolution` | resolution | `100ppi` |  | Kura default: 100 ppi resolves details of about 0.25 mm, finer than ink-limit problems that matter on press. |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |

#### `print.ink_coverage`: Total ink coverage

Highest total ink amount (sum of all inks, in percent) on the page, as rendered.

- Value: `percent`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: page
- References: ISO 12647-2 5.3; Ghent Workgroup specification 2015
- Notes: Typical limits: 300% sheet-fed coated, 260% newsprint.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `min_area` | length | `1mm` |  | Only peaks covering at least a square of this side count. Kura default: a spot of at least 1 mm x 1 mm is visible on press; smaller peaks are ignored. |
| `sample_resolution` | resolution | `100ppi` |  | Kura default: 100 ppi resolves details of about 0.25 mm, finer than ink-limit problems that matter on press. |
| `area` | text | empty |  | Limit to a rectangle 'x y width height' in the box's coordinates. Empty: the whole page. |
| `box` | enum | `media` | media, crop, bleed, trim, art | The whole sheet is examined unless a box is chosen. |

#### `print.mixing_hints`: DeviceN mixing hints consistency

Printing order and solidity values in DeviceN/NChannel mixing hints agree across colour spaces.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 8.6.6.5
- Notes: Values: printing_order_inconsistent, solidities_inconsistent, printing_order_missing, solidities_missing.

No parameters.

#### `print.plates`: Separations

The colour separations (plates) the page or document will print on.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 8.6.6; ISO 15930-7 6.2.4
- Notes: Use 'count' to compare the number of plates.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `page` | page, document |  |
| `kind` | enum | `all` | all, process, spot |  |
| `non_empty` | bool | `false` |  | Only plates that actually receive ink. Off: every plate referenced counts. |

#### `print.registration_used`: Registration colour prints

Content in the All (registration) separation actually leaves ink.

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: ISO 32000-2 8.6.6.4

No parameters.

#### `print.rendering_functions`: Device-dependent rendering functions

The graphics state sets transfer, black-generation or undercolour-removal functions.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 10.3; ISO 32000-2 10.4; ISO 19005-2 6.2.5; ISO 15930-7 6.3
- Notes: Values: transfer, transfer_identity, transfer2_default, black_generation, black_generation2_default, undercolour_removal, undercolour_removal2_default, halftone_transfer_not_allowed.

No parameters.

#### `print.spot_naming`: Spot colour naming problems

Spot colours whose names or definitions disagree across the page or document.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 8.6.6.4; ISO 15930-7 6.2.4
- Notes: Values: same_name_different_look (one name, different alternate colours), similar_names (names that differ only in case, spaces or suffix), process_defined_differently (process colours given as Separation/DeviceN with different looks), same_look_different_names.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `document` | page, document |  |

#### `print.trap_network`: Trap network annotations

State of trap-network annotations that carry pre-built traps.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 14.11.6; ISO 15930-7 6.4
- Notes: Values: present, invalid, font_fauxed_list_not_empty, process_model_not_cmyk, process_model_missing.

No parameters.

#### `print.trapped`: Trapped flag

The Trapped declaration from the information dictionary or XMP.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 14.11.6; ISO 15930-7 6.4
- Notes: Values: True, False, Unknown, missing.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `source` | enum | `any` | any, info, xmp |  |

#### `print.used_area`: Area that carries ink

Bounding box of everything that leaves ink on the page, compared with a box or size.

- Value: `size`
- Operators: `==`, `!=`, `fits_within`, `exceeds`
- Applies to: page
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `tolerance` | length | `1pt` |  |  |

#### `print.uses_plates`: Page prints on given plates

The page puts ink on any of the listed plates (process names or spot names).

- Value: `bool`
- Operators: `is`
- Applies to: page
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `plates` | list | required |  |  |
| `min_ink` | percent | `0%` |  |  |
| `ignore_white` | bool | `true` |  | White and None separations leave no ink. |

### Page content

Paths, text, images and shadings as objects: kind, painting, position and relations.

#### `content.box_relation`: Object position against a page box

Whether the object or annotation lies inside, outside, or across a page box.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 14.11.2; ISO 32000-2 12.5.2
- Notes: Values: inside, outside, crosses. With subject annotation every annotation's Rect is tested; combine with annotation.flags to limit it to printed annotations.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `also` | enum | empty | media, crop, bleed, trim, art | Second box that must give the same answer. Empty: one box only. |
| `subject` | enum | `object` | object, annotation | What is tested: page objects, or annotation rectangles (ISO 32000-2 12.5.2). Page content is the usual subject. |

#### `content.clipped`: Clipping

How much of the object the clipping path hides.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.5.4
- Notes: Values: none, partly, fully.

No parameters.

#### `content.context`: Where the object sits

Containers the object is drawn inside.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 8.10; ISO 32000-2 8.7.3
- Notes: Values: form_xobject, tiling_pattern, type3_glyph, compatibility_section, annotation_appearance.

No parameters.

#### `content.contrast_sufficient`: Enough contrast against the background

Contrast between the colour of text or a graphic and the rendered colour right behind it meets the minimum for its kind.

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: WCAG 2.2 1.4.3; WCAG 2.2 1.4.11
- Notes: The page is rendered once; the background is the most frequent colour in the text box (for text) or in a thin ring around the shape (for graphics) that differs from the object's own colour. Colours, including CMYK and spot colours, are turned into relative luminance through the colour engine. Invisible text, whitespace, artifacts, hidden layers, shadings, page-sized shapes and shapes that cannot be seen at all (contrast below 1.05) are skipped. Bold means a bold font name, weight 600 or more, or the ForceBold flag.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `kind` | enum | `text` | text, graphics, any | Text contrast is the requirement most documents are measured against. |
| `normal_text_ratio` | ratio | `4.5` |  | WCAG 2.2 1.4.3 minimum for normal text. |
| `large_text_ratio` | ratio | `3` |  | WCAG 2.2 1.4.3 minimum for large text. |
| `graphics_ratio` | ratio | `3` |  | WCAG 2.2 1.4.11 minimum for graphics. |
| `large_size` | length | `18pt` |  | WCAG treats 18pt text as large. |
| `large_bold_size` | length | `14pt` |  | WCAG treats 14pt bold text as large. |
| `resolution` | resolution | `96ppi` |  | Enough to find the background colour; text shapes need not be resolved. |

#### `content.dash_length`: Dash and gap lengths

Effective lengths of dashes or gaps after transformation.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `part` | enum | `dash` | dash, gap |  |
| `measure` | enum | `average` | average, longest, shortest |  |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |

#### `content.distance_to_box`: Distance to a box edge

Smallest distance between the object and a page box edge.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura
- Notes: Inside distances find content too close to the trim (the safety margin); outside distances find content beyond it.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `side` | enum | `outside` | outside, inside |  |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |

#### `content.distorted`: Distorted placement

The object is scaled differently horizontally and vertically.

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: Kura

No parameters.

#### `content.in_area`: Object in an area

The object lies at a point or inside, outside or across a rectangle you give.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: Kura
- Notes: Values: inside, outside, crosses, at (the reference point lies on the object).

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `x` | length | required |  |  |
| `y` | length | required |  |  |
| `width` | length | `0mm` |  | Zero: a point. |
| `height` | length | `0mm` |  |  |
| `origin` | enum | `media` | media, crop, bleed, trim, art, lower_left, center |  |
| `tolerance` | length | `0.5mm` |  | Kura default: half a millimetre covers placement rounding. |

#### `content.kind`: Object kind

What kind of page object this is.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.2
- Notes: Values: path, text, image, image_mask, shading, form_xobject. 'vector' in Kura profiles means path or shading.

No parameters.

#### `content.outside_share`: Share of the object outside a box

Percentage of the object's area that lies outside a page box.

- Value: `percent`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |

#### `content.painting`: How the object is painted

Whether a path or text is filled, stroked, both, or neither.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.5.3; ISO 32000-2 9.3.6
- Notes: Values: fill, stroke, fill_and_stroke, none.

No parameters.

#### `content.path_measure`: Path geometry

Counts and sizes of a path's segments.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 8.5.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `measure` | enum | `points` | points, subpaths, open_subpaths, closed_subpaths, straight_segments, curves, rectangles, largest_subpath_points, smallest_subpath_points, consecutive_points, single_point, page_points |  |

#### `content.relation`: Relation to other objects

How the object relates to other objects on the page, in painting order and in space.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: Kura
- Notes: Values: above (painted over another object it touches), below, covers (completely hides another), covered, crosses, inside, outside, near.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `other` | text | empty |  | Name of a check that selects the objects to compare with. Empty: any other object. |
| `distance` | length | `0.5mm` |  | Kura default: objects closer than half a millimetre are hard to separate in print. |
| `min_objects` | count | `1` |  |  |

#### `content.shading`: Shading properties

Type and validity of a smooth shading.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 8.7.4.5
- Notes: type: function, axial, radial, free_form, lattice, coons, tensor.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `type` | type, radial_without_radius, bbox_valid |  |

#### `content.shape_relation`: Relation to a shape

How the object relates to a reference shape: a page box or the outline of selected objects, optionally grown or shrunk.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: Kura
- Notes: Values: inside, outside, crosses, in_edge_band.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `shape` | text | `trim` |  | A box name or the name of a check selecting outline objects. The trim box is the usual reference. |
| `offset` | length | `0mm` |  | Grow (positive) or shrink (negative) the shape. |
| `edge_band` | length | `2mm` |  | Kura default: content within 2 mm of a cut is at risk of being trimmed (half of a 3 mm bleed plus tolerance). |

#### `content.sharp_corners`: Sharp corners and tight curves

Number of corners sharper than an angle or curves tighter than a radius, which cutting plotters cannot follow.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `max_angle` | angle | `30` |  | Kura default: corners under 30 degrees tear or lift in die cutting. |
| `min_radius` | length | `0mm` |  |  |
| `scope` | enum | `object` | object, page |  |

#### `content.size`: Object size

Width, height or area of the object's bounding box.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `dimension` | enum | `area` | width, height, area, shorter, longer |  |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |

#### `content.stroke_geometry`: Stroke geometry

Length, angle or corner radius of a stroked path.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `measure` | enum | `length` | length, angle, corner_radius |  |
| `unit` | enum | `mm` | pt, mm, cm, in | Lengths are evaluated in points internally; the unit only changes how the value is written. |

#### `content.stroke_style`: Line style

Line cap, join, miter limit, dash pattern and stroke adjustment.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 8.4.3
- Notes: pattern is solid, dashed, dotted or mixed.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `pattern` | cap, join, miter_limit, dash_array, dash_phase, pattern, stroke_adjustment |  |

#### `content.stroke_width`: Line width

Width of a stroke after the current transformation, in the chosen unit.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 8.4.3.2
- Notes: Hairline limits: 0.125 pt is a common minimum for offset printing.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `unit` | enum | `pt` | pt, mm, cm, in | Line weights are usually stated in points. |
| `ignore_white` | bool | `false` |  | Ignore strokes that leave no ink. |

#### `content.visibility`: Visible result

Whether the object can be seen once everything drawn after it is considered.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: Kura
- Notes: Values: visible, hidden (fully covered, clipped away or painted without ink), partly_covered.

No parameters.

### Form and reference XObjects

Reusable forms and external references.

#### `xobject.form_problem`: Form XObject problems

Rule violations in form XObjects.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 8.10; ISO 19005-2 6.2.9
- Notes: Values: inherits_resources, marked_content_ids_not_referenced, contains_referenced_marked_content.

No parameters.

#### `xobject.reference`: Reference XObject

Properties of a form that references external content (PDF/X-5).

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 8.10.4; ISO 15930-8 6
- Notes: Values: has_id, has_metadata, metadata_document_id, metadata_document_id_uuid, metadata_version_id, metadata_rendition_class, metadata_rendition_of, circular.

No parameters.

### Colour

Colour spaces, ink amounts, overprint and spot colours of objects.

#### `color.black_point_compensation`: Black point compensation setting

The graphics state's black point compensation entry.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.4.5
- Notes: Values: absent, ON, OFF, Default.

No parameters.

#### `color.channel_spread`: Difference between inks

Difference in percent between the strongest and the weakest component.

- Value: `percent`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |

#### `color.component_count`: Number of colour components

Components of the colour space, or of its alternate space.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 8.6

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |
| `level` | enum | `direct` | direct, alternate |  |

#### `color.device_space_rule`: Device colour without a defined meaning

How a device colour space gets its meaning, for the rules of PDF/A and PDF/X.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 19005-2 6.2.4.3; ISO 32000-2 8.6.5.6; ISO 32000-2 11.6.6
- Notes: Values: default_space, group_blend_space_pdf2, cmyk_redefined_by_group, matches_pdfa_intent, matches_pdfx_intent, matches_pdfe_intent, matches_blend_space, cmyk_profile_equals_pdfa_intent, cmyk_profile_equals_pdfx_intent, cmyk_icc_equals_blend_space, icc_component_mismatch, base_space_of_entry.

No parameters.

#### `color.devicen`: DeviceN and NChannel details

Properties of DeviceN and NChannel colour spaces.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 8.6.6.5; ISO 19005-2 6.2.4.4
- Notes: Values: duplicate_colorant, process_as_devicen, has_colorants_dict, has_mixing_hints, has_process_dict, missing_process_colorants, has_process_key, subtype_nchannel. Use color.devicen_colorants for names and counts.

No parameters.

#### `color.devicen_colorants`: DeviceN colorant names

Colorant names of a DeviceN space, its process colour space, or remapped colorants.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `which` | enum | `all` | all, process, remapped |  |

#### `color.ink`: Amount of one ink

Percentage of a process ink (cyan, magenta, yellow, black) or of spot inks the object uses; 0 when the ink is not used.

- Value: `percent`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura
- Notes: max is the strongest ink of the object; cmy_max the strongest of cyan, magenta and yellow (used for rich black).

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |
| `ink` | enum | `black` | cyan, magenta, yellow, black, cmy_max, any_spot, max |  |

#### `color.ink_mix`: Ink combination

Which inks the object uses.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: Kura
- Notes: Values: none (white), black_only, solid_black (100% K only), cmy_only, neutral_cmy (equal C, M and Y), single_process, process_only, spot_only, spot_and_black, process_and_spot, registration, additive. Images are classified by the inks of their colour space.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |

#### `color.inks_used`: Number of inks used

How many inks receive a non-zero amount.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |
| `kind` | enum | `any` | any, process, spot |  |

#### `color.overprint`: Overprint

Whether overprint is on for the fill or the stroke, and the overprint mode.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.6.7; ISO 32000-2 11.7.4
- Notes: Values: off, on, on_nonzero_mode (OPM 1).

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |

#### `color.rendering_intent`: Rendering intent

Rendering intent in effect, and whether it was set explicitly.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.6.5.8
- Notes: Values: Perceptual, RelativeColorimetric, Saturation, AbsoluteColorimetric.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `explicit_only` | bool | `false` |  |  |

#### `color.space`: Colour space

Colour space family of the object's colour.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.6
- Notes: Values: DeviceGray, DeviceRGB, DeviceCMYK, CalGray, CalRGB, Lab, ICCBased, Indexed, Pattern, Separation, DeviceN, NChannel; 'device' and 'device_independent' are accepted in lists as shorthands.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |
| `level` | enum | `direct` | direct, base, alternate, icc_alternate, process | direct: as named; base: under Indexed; alternate: of Separation/DeviceN; icc_alternate: an ICC profile's /Alternate; process: of a DeviceN /Process dictionary. The space named in the content stream. |
| `icc_components` | count | empty |  | For ICCBased: number of profile components (1 gray, 3 RGB, 4 CMYK). Empty: any ICC profile. |

#### `color.spot`: Spot colour

Name and kind of a Separation or DeviceN colorant.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 8.6.6.4
- Notes: kind: spot, registration (All), none (None), process (a process ink name used as a spot). colour_system: pantone, hks or other, from the name suffix.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `name` | name, kind, colour_system, name_encoding_valid, alternate_value |  |
| `component` | count | `1` |  | For alternate_value: which alternate component (1-4). |

#### `color.total_ink`: Total ink of the object

Sum of all ink percentages of the object's colour.

- Value: `percent`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 12647-2 5.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |
| `process_only` | bool | `false` |  |  |

#### `color.unused_resource`: Unused colour resources

Device colour spaces or blend modes referenced from resources but never used.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 19005-2 6.2.4.3
- Notes: Values: DeviceGray, DeviceRGB, DeviceCMYK, blend_mode.

No parameters.

#### `color.value`: Colour value

The colour values of the object in percent (0-100), one component or all.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 8.6

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |
| `component` | count | empty |  | Empty: all components as a space-separated list. |

### ICC profiles

Headers and tags of ICC profiles used by objects and output intents.

#### `icc.header`: ICC profile header field

A field of an ICC profile header, for profiles used by objects or by output intents.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 15076-1 7.2; ISO 19005-2 6.2.4.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `field` | enum | `version` | version, colour_space, device_class, pcs, rendering_intent, size, cmm, creator, manufacturer, model, platform, flags, attributes, date, profile_id, illuminant_x, illuminant_y, illuminant_z, signature_valid |  |
| `profile` | enum | `object` | object, output_intent, output_intent_pdfa, output_intent_pdfe |  |

#### `icc.matches_output_intent`: Profile agrees with an output intent

True when an object's embedded ICC profile matches an output intent profile, either exactly or by colour space or device class.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 15930-7 6.2.4; ISO 19005-2 6.2.4.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `intent` | enum | `pdfx` | pdfx, pdfa, pdfe, destination |  |
| `field` | enum | `profile` | profile, colour_space, device_class | What must agree: the whole profile, its colour space or its device class. |

#### `icc.max_ink`: Ink limit of a profile

Highest total ink a CMYK profile produces, in percent.

- Value: `percent`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `profile` | enum | `object` | object, output_intent, output_intent_pdfa, output_intent_pdfe |  |

#### `icc.tag`: ICC profile tag

Text of a descriptive tag in an ICC profile.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 15076-1 9

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `tag` | enum | `description` | description, copyright, calibration_date, device_manufacturer, device_model, technology, viewing_conditions, screening, rendering_info, named_colours |  |
| `profile` | enum | `object` | object, output_intent, output_intent_pdfa, output_intent_pdfe |  |

### Transparency

Opacity, blend modes, soft masks and transparency groups.

#### `transparency.alpha`: Constant opacity

Constant alpha for fill or stroke (1 = opaque).

- Value: `number`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 11.6.4.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |

#### `transparency.blend_mode`: Blend mode

Blend mode in effect.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 11.3.5; ISO 19005-2 6.2.10
- Notes: Values: the standard blend mode names; 'separable' and 'non_separable' are list shorthands.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `inherited` | bool | `false` |  | Report every mode in effect: the object's own and those of the enclosing forms and groups (one entry each). |

#### `transparency.group`: Transparency group

Properties of the transparency group the object belongs to.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 11.6.6; ISO 19005-2 6.2.10
- Notes: Values: member, isolated, knockout, text_knockout, has_colour_space, device_independent_blend_space.

No parameters.

#### `transparency.group_colour_space`: Group blending colour space

Blending colour space of the group, or of the outermost group.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 11.6.6

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `which` | enum | `nearest` | nearest, outermost, base, luminosity_mask |  |

#### `transparency.group_depth`: Group nesting depth

How deeply transparency groups are nested around the object.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

No parameters.

#### `transparency.keys_present`: Transparency keys present

The graphics state contains transparency entries, even if they have no effect.

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: ISO 19005-1 6.4

No parameters.

#### `transparency.soft_mask`: Soft mask properties

Properties of a soft mask.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 11.6.5.2
- Notes: Values: alpha, luminosity, backdrop_colour, transfer_function, identity_transfer; 'none' when the graphics state sets SMask to /None.

No parameters.

#### `transparency.used`: Object is transparent

The object is drawn with transparency: a soft mask, constant alpha below 1 or a non-Normal blend mode.

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: ISO 32000-2 11; ISO 15930-1 6.3; ISO 19005-1 6.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `paint` | enum | `any` | any, fill, stroke | Any: the fill for filled objects and the stroke for stroked ones. |
| `how` | enum | `any` | any, soft_mask, alpha, blend_mode, image_soft_mask |  |

### Text

Text size, rendering, orientation and Unicode mapping.

#### `text.geometry`: Text orientation

Rotation and slant of the text.

- Value: `angle`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura
- Notes: Slant detects synthetic italics made by skewing upright glyphs.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `measure` | enum | `rotation` | rotation, slant |  |

#### `text.glyph`: Glyph state

State of the glyph drawn for a character.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 9.6; ISO 19005-2 6.2.11.8
- Notes: Values: normal, undefined (code not defined in a font that is not embedded), notdef (code missing from the embedded font program, shown as .notdef), whitespace, empty_non_space (non-space glyph without outline), has_actual_text.

No parameters.

#### `text.legacy_codes`: Legacy character codes

Text uses character codes outside the font's declared encoding range.

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: Kura

No parameters.

#### `text.negative_size`: Negative font size

The font size operand is negative (text is mirrored).

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: ISO 32000-2 9.3.2

No parameters.

#### `text.rendering`: How text is rendered

Rendering of text derived from the render mode.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 9.3.6
- Notes: Values: fill, stroke, fill_and_stroke, invisible, clip_only.

No parameters.

#### `text.size`: Text size

Font size of the text as it appears on the page, after scaling.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 9.3; ISO 32000-2 9.4.4
- Notes: x_height uses the font's measured x-height; x_height_percent is x-height relative to the font size.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `measure` | enum | `font_size` | font_size, x_height, x_height_unscaled, x_height_percent | Font size is what designers specify. |
| `unit` | enum | `pt` | pt, mm, cm, in | Type is measured in points. |

#### `text.state`: Text state value

A text state parameter in effect for the text.

- Value: `number`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 9.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `parameter` | enum | `render_mode` | render_mode, character_spacing, word_spacing, horizontal_scaling, leading, rise |  |

#### `text.type3_glyph_operator`: Type 3 glyph metrics operator

Which glyph-width operator (d0 or d1) a Type 3 glyph procedure uses.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 9.6.4
- Notes: Values: d0, d1.

No parameters.

#### `text.unicode`: Unicode mapping

How characters map to Unicode.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 9.10; ISO 19005-2 6.2.11.7; ISO 14289-1 7.21.7
- Notes: Values: mapped, unmapped, private_use, ligature, zero, inappropriate, ambiguous (one glyph maps to several values).

No parameters.

#### `text.unicode_conflict`: Unicode mapping disagrees

The ToUnicode map disagrees with the font's own encoding or with the PDF encoding, or a ligature mapping does not match the encoding.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 9.10.2
- Notes: Values: font_cmap, pdf_encoding, ligature.

No parameters.

### Fonts

Font type, embedding, licence, encoding and font program problems.

#### `font.all_text_unicode`: All text of the font maps to Unicode

Every character drawn with the font can be mapped to Unicode.

- Value: `bool`
- Operators: `is`
- Applies to: font
- References: ISO 19005-2 6.2.11.7; ISO 14289-1 7.21.7

No parameters.

#### `font.embedding`: Font embedding

Whether the font program is embedded, and whether it is a subset.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: font
- References: ISO 32000-2 9.9; ISO 19005-2 6.2.11.4; ISO 15930-7 6.3
- Notes: Values: not_embedded, embedded_subset, embedded_full.

No parameters.

#### `font.encoding`: Font encoding

Encoding the font uses.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: font
- References: ISO 32000-2 9.6.5; ISO 32000-2 9.7.5
- Notes: Values: standard, win_ansi, mac_roman, mac_expert, custom_differences, identity_h, identity_v, cmap_predefined, cmap_embedded, builtin.

No parameters.

#### `font.flags`: Font descriptor flags

Style flags set in the font descriptor.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: font
- References: ISO 32000-2 9.8.2
- Notes: Values: fixed_pitch, serif, symbolic, script, nonsymbolic, italic, all_cap, small_cap, force_bold.

No parameters.

#### `font.has_to_unicode`: Font has ToUnicode map

The font carries a ToUnicode CMap.

- Value: `bool`
- Operators: `is`
- Applies to: font
- References: ISO 32000-2 9.10.3

No parameters.

#### `font.license`: Embedding licence

What the font's licence flags (OS/2 fsType) allow.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: font
- References: ISO/IEC 14496-22 5.2.7
- Notes: Values: installable, editable, preview_print, restricted, bitmap_only, no_subsetting, flag_missing (embedded program without readable flags), unknown_value. Only embedded fonts with a font program have a value; Type 3 fonts have none.

No parameters.

#### `font.metric`: Font metric

A metric from the font descriptor.

- Value: `number`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: font
- References: ISO 32000-2 9.8.1

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `metric` | enum | `x_height` | x_height, stem_h, avg_width, max_width, bbox_left, bbox_bottom, bbox_right, bbox_top, largest_cid |  |

#### `font.name`: Font name

The font's PostScript name, without subset prefix.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: font
- References: ISO 32000-2 9.6.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `source` | enum | `pdf` | pdf, embedded_program |  |

#### `font.name_problem`: Font name problems

Problems with the font name.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: font
- References: ISO 32000-2 9.6.2
- Notes: Values: differs_from_program, not_unique (same name, different fonts), not_utf8, bad_escaping.

No parameters.

#### `font.problem`: Font program and dictionary problems

Rule violations in a font's dictionaries or program, as checked by the archive and print standards.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: font
- References: ISO 32000-2 9; ISO 19005-2 6.2.11; ISO 14289-1 7.21
- Notes: Values: invalid_program, truetype_errors, missing_glyph, subset_missing_glyph, width_mismatch, metrics_missing, empty_bbox, glyph_overlapping_paths, identical_glyphs_for_different_chars, glyph_names_not_in_standard_list, cidset_missing, cidset_incomplete, charset_missing, charset_incomplete, cidtogidmap_missing, cid_system_info_mismatch, cid_registry_mismatch, cid_ordering_mismatch, cid_supplement_too_low, wmode_mismatch, cmap_not_embedded, cmap_bad_syntax, cmap_references_unknown, to_unicode_bad_syntax, symbolic_truetype_has_encoding, symbolic_truetype_cmap_count, symbolic_truetype_no_3_0_cmap, nonsymbolic_truetype_bad_encoding, nonsymbolic_truetype_differences, nonsymbolic_truetype_no_3_1_cmap, cmap_subtables, type3_missing_resources, font_xmp_missing_fields, has_xmp.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `strict` | bool | `false` |  | Also report problems viewers tolerate. Off: only problems that affect rendering. |

#### `font.type`: Font type

Technology of the font.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: font
- References: ISO 32000-2 9.6; ISO 32000-2 9.7
- Notes: Values: type1, multiple_master, truetype, type3, cid_type0, cid_truetype, opentype_cff, opentype_truetype, cff, standard_14. Lists cover combinations such as 'any CID font'.

No parameters.

### Images

Resolution, compression, masks, JPEG 2000, JBIG2 and OPI data of images.

#### `image.abbreviated_filter_names`: Abbreviated names in inline images

An inline image uses abbreviated keys or filter names.

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: ISO 32000-2 8.9.7

No parameters.

#### `image.alternates`: Alternate images

The image has alternate versions, and their properties.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 8.9.5.4; ISO 19005-2 6.2.8
- Notes: Values: present, default_for_printing, same_size_and_position, uses_optional_content.

No parameters.

#### `image.jbig2`: JBIG2 properties

Segment types and options used by a JBIG2 image.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO/IEC 14492; ISO 32000-2 7.4.7
- Notes: Values: globals, lossy, symbol_dictionary, pattern_dictionary, text_region, halftone_region, generic_region, generic_refinement, each with immediate_, immediate_lossless_ and intermediate_ variants where the format distinguishes them.

No parameters.

#### `image.jpeg2000`: JPEG 2000 properties

Properties of a JPEG 2000 (JPX) image.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 15444-1; ISO 19005-2 6.2.8.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `channels` | channels, colour_specs, colour_method, approximate_specs, same_bit_depth, jab_colour_space |  |

#### `image.opi`: OPI comment value

A value from an image's OPI 1.3 or 2.0 dictionary, used to swap low-resolution placeholders for print images.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 14.11.7; OPI 2.0

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `version` | enum | `2.0` | 1.3, 2.0 |  |
| `field` | enum | `file` | file, id, comments, type, colour, colour_c, colour_m, colour_y, colour_k, colour_name, crop_left, crop_top, crop_right, crop_bottom, size_width, size_height, position_llx, position_lly, position_lrx, position_lry, position_ulx, position_uly, position_urx, position_ury, resolution_x, resolution_y, tint, overprint, transparency, bits, samples, included_width, included_height, included_quality, main_image |  |

#### `image.property`: Image property

A property of an image XObject or inline image.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 8.9.5; ISO 19005-2 6.2.8
- Notes: colour_type is colour, grey, duotone or mask.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `bits_per_component` | bits_per_component, width, height, filter, lossy, compressed, ascii_encoded, interpolate, interpolate_present, intent_present, is_xobject, stencil_mask, colour_key_mask, explicit_mask, soft_mask, soft_mask_in_data, soft_mask_in_data_value, soft_mask_matte, length_correct, invalid, bytes_compressed, bytes_uncompressed, compression_ratio, inline_size, jpeg_quality, opi_info, colour_type |  |
| `alternate` | bool | `false` |  | Examine the image's alternate image instead (Alternates array). |

#### `image.resolution`: Effective image resolution

Resolution of the image as placed on the page, in pixels per inch.

- Value: `resolution`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 32000-2 8.9; Ghent Workgroup specification 2015
- Notes: Typical minimums: 225-300 ppi for colour and grey, 550-1200 ppi for 1-bit images.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `axis` | enum | `lowest` | lowest, highest, horizontal, vertical | The lower of the two directions decides print quality. |
| `kind` | enum | `any` | any, colour_or_grey, bitmap |  |

#### `image.rotation`: Image rotation

Rotation of the image on the page.

- Value: `angle`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `right_angles_only` | bool | `false` |  | Report only rotations that are not multiples of 90 degrees. |

### Layers

Optional content groups, configurations and processing steps.

#### `layer.configuration`: Layer configuration problems

Rule violations in the optional content properties and configurations.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 8.11.4.3; ISO 19005-2 6.9; ISO 15930-7 6.6
- Notes: Values: no_default_config, has_alternate_configs, config_missing_name, config_name_not_unique, has_base_state, base_state_off, base_state_on, has_off_array, missing_order, empty_order, order_missing_layers, has_as_entry.

No parameters.

#### `layer.configuration_name`: Configuration name and order

Names of the configurations and of layers listed in their Order arrays.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `which` | enum | `configuration` | configuration, order |  |

#### `layer.membership`: Layer membership

Which layers (optional content groups) the object belongs to.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 32000-2 8.11
- Notes: Use 'count' for how many layers; an empty list means the object is on no layer.

No parameters.

#### `layer.processing_step`: Processing step layers

Layers marked as processing steps (die lines, varnish, white, braille and so on).

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 19593-1 5
- Notes: Value lists the step types of the object's layers. Problems are reported by layer.processing_step_problem.

No parameters.

#### `layer.processing_step_problem`: Processing step problems

Rule violations of the processing-steps convention.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 19593-1 5
- Notes: Values: missing_type, custom_value, custom_key_not_second_class, same_step_in_several_layers, layers_missing_on_page, spot_used_by_several_steps, spot_used_by_print_content, step_uses_several_spots, document_metadata, page_metadata.

No parameters.

#### `layer.property`: Layer property

A property of the layer (optional content group) the object belongs to.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 8.11.4.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `name` | name, has_name, creator, has_creator_info, locked, empty, in_navigation, radio_group, exported, printed, zoom_min, zoom_max, hidden_at_some_zoom, language, for_language, users, user_type, for_users, content_type, pagination_artifact, pagination_subtype, has_usage_user, metadata, listed | listed: whether the layer appears in the document's list of layers (OCProperties OCGs); false finds layers that are used but not declared. |
| `scope` | enum | `object` | object, document | object: the layers the object belongs to; document: every layer of the file (one hit when any matches), e.g. to find empty layers. The layers of the object being checked. |

#### `layer.visibility_policy`: Membership visibility policy

Visibility policy of an optional content membership dictionary.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 32000-2 8.11.2.2
- Notes: Values: AllOn, AnyOn, AnyOff, AllOff.

No parameters.

#### `layer.visible`: Layer visibility

Whether the object is visible in the default configuration, in any configuration, or in the current view.

- Value: `bool`
- Operators: `is`
- Applies to: object
- References: ISO 32000-2 8.11.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `in` | enum | `default` | default, any_configuration, current |  |

### Annotations

Annotation types, flags, appearances and entries.

#### `annotation.appearance`: Appearance stream problems

Rule violations of the appearance dictionary.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: annotation
- References: ISO 32000-2 12.5.5; ISO 19005-2 6.3.3
- Notes: Values: missing, has_other_than_normal, normal_has_several_states, normal_is_dictionary, needs_appearances_flag.

No parameters.

#### `annotation.entry`: Annotation entry

Value or presence of an entry of the annotation dictionary.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: annotation
- References: ISO 32000-2 12.5.2; ISO 19005-2 6.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `entry` | enum | `contents` | contents, name, opacity, has_opacity, blend_mode, has_blend_mode, colour, colour_is_rgb, interior_colour, interior_colour_is_rgb, has_appearance, tab_order, has_tab_order |  |

#### `annotation.flags`: Annotation flags

Flags set on the annotation.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: annotation
- References: ISO 32000-2 12.5.3; ISO 19005-2 6.3.2
- Notes: Values: invisible, hidden, print, no_zoom, no_rotate, no_view, read_only, locked, toggle_no_view, locked_contents; 'missing' when there is no F entry.

No parameters.

#### `annotation.interactive`: Interactive annotation

The annotation is interactive (not a print-only mark).

- Value: `bool`
- Operators: `is`
- Applies to: annotation
- References: Kura

No parameters.

#### `annotation.link_colour_only`: Link marked by colour only

Link text differs from the text next to it only in colour.

- Value: `bool`
- Operators: `is`
- Applies to: annotation
- References: WCAG 2.2 1.4.1
- Notes: Best effort: true when the link text has a different colour from the nearest text on the same line, the same font, no underline or border, and less than 3:1 contrast with that text. Links without neighbouring text give no value.

No parameters.

#### `annotation.link_text`: Link text

The words shown inside a link's area, in lower case without surrounding punctuation.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: annotation
- References: WCAG 2.2 2.4.4; WCAG 2.2 2.4.9
- Notes: Compare with in or matches to find links that do not say where they go, e.g. in [click here, here, read more, more, link, this] with case_sensitive false. Links without text (image links) give no value.

No parameters.

#### `annotation.minimum_pdf_version`: PDF version an annotation type needs

The lowest PDF version that defines the annotation's subtype.

- Value: `version`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: annotation
- References: ISO 32000-2 12.5.6
- Notes: Uses the PDF version that introduced each annotation subtype.

No parameters.

#### `annotation.multimedia`: 3D and media annotation problems

Rule violations in 3D and multimedia annotation data.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: annotation
- References: ISO 32000-2 13.6; ISO 32000-2 13.2; ISO 24517-1 6.6
- Notes: Values: 3d_colour_space, 3d_device_rgb, 3d_stream_resources, 3d_stream_subtype, media_clip_missing_alt, media_clip_missing_type, media_has_mh.

No parameters.

#### `annotation.outside_crop`: Annotation outside the visible page

The annotation rectangle lies outside the crop box, or has zero size.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: annotation
- References: Kura
- Notes: Values: outside, zero_size.

No parameters.

#### `annotation.type`: Annotation type

Subtype of the annotation.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: annotation
- References: ISO 32000-2 12.5.6
- Notes: Values: the standard subtype names (Link, Text, Widget, ...). 'unknown' marks subtypes ISO 32000 does not define.

No parameters.

### Forms

Interactive form fields and XFA.

#### `form.field_flags`: Form field flags

Field flags set on a form field.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: annotation
- References: ISO 32000-2 12.7.4
- Notes: Values: read_only, required, no_export, multiline, password, no_toggle_to_off, radio, push_button, combo, edit, sort, file_select, multi_select, do_not_spell_check, do_not_scroll, comb, radios_in_unison, commit_on_change; 'missing' when there is no Ff entry.

No parameters.

#### `form.field_name`: Form field name

One of the field's names.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: annotation
- References: ISO 32000-2 12.7.4.2; ISO 14289-1 7.18.1

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `which` | enum | `full` | full, partial, alternate, export |  |

#### `form.field_type`: Form field type

Type of a form field.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: annotation
- References: ISO 32000-2 12.7.4
- Notes: Values: text, check_box, radio, push_button, combo, list, signature.

No parameters.

#### `form.field_value`: Form field value

Current value of the field.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: annotation
- References: ISO 32000-2 12.7.4

No parameters.

#### `form.xfa`: XFA form

The interactive form carries XFA data, optionally dynamic XFA that replaces the PDF pages.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 12.7.9; ISO 19005-2 6.4.2
- Notes: Values: none, static, dynamic.

No parameters.

### Actions and links

Actions, triggers, destinations and link targets.

#### `action.broken_destination`: Destination points nowhere

A link or bookmark points to a destination that does not exist.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 12.3.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `from` | enum | `any` | any, link, bookmark |  |

#### `action.destination`: Destination fit mode

Fit mode of destinations used by links, bookmarks and GoTo actions.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 12.3.2.2
- Notes: Values: xyz, fit, fith, fitv, fitr, fitb, fitbh, fitbv.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `from` | enum | `any` | any, link, bookmark, action |  |

#### `action.present`: Action present

The document, a page, an annotation or a form field triggers an action of the selected type.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 12.6; ISO 19005-2 6.5; ISO 14289-1 7.19

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `type` | enum | `any` | any, goto, goto_remote, goto_embedded, goto_3d_view, launch, thread, uri, sound, movie, hide, named, submit_form, reset_form, import_data, javascript, set_ocg_state, rendition, transition, rich_media |  |
| `on` | enum | `any` | any, document, page, annotation, form_field, outline |  |
| `trigger` | enum | `any` | any, activate, open, additional |  |

#### `action.trigger`: Additional-action trigger

Event name of additional actions (for example O, C, K, F, V).

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 12.6.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `on` | enum | `any` | any, document, page, annotation, form_field |  |

#### `action.type`: Action type

Type of each action found.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 12.6.4
- Notes: Values: goto, goto_remote, goto_embedded, goto_3d_view, launch, thread, uri, sound, movie, hide, named, submit_form, reset_form, import_data, javascript, set_ocg_state, rendition, transition, rich_media.

No parameters.

#### `action.uri`: Link target URI

URI opened by a link annotation.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: annotation
- References: ISO 32000-2 12.6.4.8

No parameters.

### Embedded files

Embedded and associated files.

#### `attachment.count`: Number of embedded files

How many files are embedded, optionally limited to where they are attached and whether they are associated files.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 7.11.4; ISO 32000-2 14.13; ISO 19005-3 6.8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `attached_to` | enum | `any` | any, document, page, page_object, annotation, form_xobject, image, structure_element, marked_content, document_part |  |
| `associated` | enum | `any` | any, yes, no |  |

#### `attachment.is_pdf`: Embedded file is a PDF

An embedded file is itself a PDF.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-2 6.8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `conforms_to` | enum | empty | pdfa-1a, pdfa-1b, pdfa-2a, pdfa-2b, pdfa-2u, pdfa-3a, pdfa-3b, pdfa-3u, pdfa-4, pdfa-4e, pdfa-4f, pdfx-1a, pdfx-3, pdfx-4, pdfx-4p, pdfx-5g, pdfx-5n, pdfx-5pg, pdfx-6, pdfx-6n, pdfx-6p, pdfe-1, pdfvt-1, pdfvt-2, pdfvt-3, pdfua-1 | Empty: any PDF. |

#### `attachment.problem`: Embedded file problems

Rule violations for embedded files and their file specifications.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 7.11; ISO 19005-3 6.8; ISO 19005-4 6.9
- Notes: Values: empty_description, missing_description, missing_mime_type, invalid_mime_type, missing_relationship, non_standard_relationship, not_associated, not_in_name_tree, params_without_mod_date, missing_stream_type, missing_f_key, missing_uf_key, af_not_array_of_file_specs, name_differs_from_pdf_name, has_related_files (RF entry).

No parameters.

#### `attachment.property`: Embedded file property

A property of an embedded file and its file specification.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 7.11.3; ISO 32000-2 7.11.4; ISO 19005-3 6.8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `name` | name, extension, description, mime_type, relationship, subtype, stream_type |  |

### Electronic invoices

Structured invoice data attached to the PDF.

#### `einvoice.metadata`: Invoice metadata in XMP

The XMP carries the invoice extension schema describing the attached invoice.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-3 6.6

No parameters.

#### `einvoice.present`: Electronic invoice attached

The document carries a structured invoice XML (EN 16931 profile) as an associated file.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: EN 16931-1; ISO 19005-3 6.8
- Notes: Counts invoice XML files.

No parameters.

#### `einvoice.relationship_valid`: Invoice relationship is valid

Every invoice file is associated with the document with an allowed relationship (Alternative, Data or Source).

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-3 6.8

No parameters.

#### `einvoice.valid`: Invoice XML is valid

The attached invoice XML is well-formed and matches its declared schema.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: EN 16931-1

No parameters.

### Digital signatures

Signature fields, dictionaries and permissions.

#### `signature.dictionary`: Signature dictionary entry

Value or presence of an entry in a signature, signature reference or transform dictionary.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 12.8.1; ISO 19005-2 6.4.3
- Notes: Presence is tested with != and an empty value; covers_whole_file is a yes/no text.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `entry` | enum | `sub_filter` | sub_filter, byte_range, covers_whole_file, has_version, digest_location, digest_method, digest_value, transform_method, transform_permissions |  |

#### `signature.flags`: Signature flags

SigFlags in the interactive form dictionary.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 32000-2 12.7.3
- Notes: Values: absent, none, signatures_exist, append_only, both.

No parameters.

#### `signature.permissions`: Document permissions dictionary

Which entries the catalog's Perms dictionary contains.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 12.8.4; ISO 19005-2 6.1.12
- Notes: Values: docmdp, ur, ur3, other.

No parameters.

#### `signature.present`: Signature fields

The document has signature fields, optionally only signed ones.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 12.7.5.5; ISO 32000-2 12.8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `signed_only` | bool | `false` |  |  |

### Kura check seal

The seal Kura adds when a file passed a profile.

#### `seal.record`: Kura check seal record

A field of the record stored with the seal.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `field` | enum | `profile_name` | result, result_text, profile_name, profile_author, profile_author_version, profile_digest, profile_format |  |

#### `seal.state`: Kura check seal

State of the seal Kura adds when it certifies that a file passed a check profile.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: Kura
- Notes: Values: absent, present, invalid_signature, invalid_metadata, modified_since_sealed, sealed_with_errors.

No parameters.

### Metadata

XMP packets and the document information dictionary.

#### `metadata.info`: Document information entry

Value of an entry in the document information dictionary.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 14.3.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `key` | text | `Title` |  | Title is the entry accessibility rules care about. |

#### `metadata.info_keys`: Information dictionary contents

Which kinds of entries the information dictionary contains.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 14.3.3; ISO 19005-4 6.6.1
- Notes: Values: standard, custom, pdfx, other_than_mod_date.

No parameters.

#### `metadata.info_matches_xmp`: Information entry agrees with XMP

An information dictionary entry has the same value as its XMP equivalent.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-1 6.7.3; ISO 19005-2 6.6.2.2; ISO 15930-7 6.5

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `key` | enum | `all` | all, Title, Author, Subject, Keywords, Creator, Producer, CreationDate, ModDate, Trapped, GTS_PDFXVersion, ISO_PDFEVersion | All: the PDF/A synchronisation rule as a whole (Title, Author, Subject, Keywords, Creator, Producer, CreationDate, ModDate). |

#### `metadata.info_problem`: Information dictionary value problems

Entries of the wrong type or encoding.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 14.3.3; ISO 15930-7 6.5
- Notes: Values: date_in_unicode, trapped_not_name, pdfx_version_not_string, pdfx_conformance_not_string, pdfx_version_unicode, pdfe_version_unicode.

No parameters.

#### `metadata.object_has_xmp`: Object carries its own XMP

A page, image, font, form, ICC profile or property list has a Metadata stream.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 32000-2 14.3.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `object` | enum | `any` | any, page, image, font, form_xobject, icc_profile, property_list |  |

#### `metadata.xmp_document_id_is_uuid`: Document ID is a UUID

xmpMM:DocumentID uses the uuid: form.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 16612-2 6.4

No parameters.

#### `metadata.xmp_extension_schema`: XMP extension schema

State of the PDF/A extension schema container that describes custom properties.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 19005-1 6.7.8; ISO 19005-2 6.6.2.3.3
- Notes: Values: absent, valid, invalid.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_history_incomplete`: Incomplete history entry

An xmpMM:History event lacks a field.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-1 6.7.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `field` | enum | `any` | any, action, instanceID, parameters, softwareAgent, when |  |
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_identification`: Standard identification in XMP

Problems in the identification schema that declares conformance to a standard.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 19005-2 6.6.4; ISO 14289-1 5; ISO 15930-7 6.5; ISO 24517-1 6.4; ISO 16612-2 6.4
- Notes: Values: missing, part_missing, wrong_namespace, revision_missing, revision_not_four_digits, unknown_properties, version_only.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `standard` | enum | `pdfa` | pdfa, pdfua, pdfx, pdfe, pdfvt |  |

#### `metadata.xmp_namespaces`: XMP namespaces

Namespace URIs used in the packet.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 16684-1 7.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_prefix`: Prefix used for a namespace

The prefix a packet binds to a given namespace URI. Some standards require the customary prefix.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 19005-1 6.7.3; ISO 19005-2 6.6.2.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `namespace` | text | `http://purl.org/dc/elements/1.1/` |  | Dublin Core is the namespace most rules mention. |
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_problem`: XMP packet problems

Problems with an XMP packet as a whole.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 16684-1 7.3; ISO 19005-1 6.7.5; ISO 19005-2 6.6.2.1
- Notes: Values: not_well_formed, filtered (not plain text), not_xmp_conformant, bytes_attribute, encoding_attribute.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_property`: XMP property value

Value of one XMP property, named by its standard prefix and name (for example dc:title or xmp:CreateDate).

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 16684-1 7; ISO 32000-2 14.3.2; IPTC Photo Metadata Standard
- Notes: Presence is tested with != and an empty value; an empty value test reports properties that exist but are empty.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `name` | text | required |  | Qualified property name such as dc:title. |
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_property_known`: XMP property is defined

Every property in the packet is defined by the selected rule set: a standard's list of predefined schemas, or the packet's own extension schemas.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-1 6.7.2; ISO 19005-2 6.6.2.3; ISO 15930-7 6.5; ISO 24517-1 6.4
- Notes: All conditions of one check are evaluated on the same property, so 'not in the standard and not in an extension schema' finds truly undefined properties.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `by` | enum | `pdfa-2` | pdfa-1, pdfa-2, pdfx-4, pdfe-1, xmp-2004, xmp-2005, extension_schema | PDF/A-2 rules cover the widest set of predefined schemas. |
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_schema_file`: XMP schema description file

State of the RELAX NG description of custom XMP properties attached to the metadata stream.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: document
- References: ISO 16684-2; ISO 19005-4 6.6
- Notes: Values: absent, valid, invalid.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

#### `metadata.xmp_size`: XMP packet size

Size of an XMP packet.

- Value: `bytes`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |
| `unit` | enum | `kb` | bytes, kb, mb |  |

#### `metadata.xmp_value_type`: XMP value matches its definition

Each property's value has the type its definition requires (text, date, closed choice, structure fields).

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005-1 6.7.2; ISO 19005-2 6.6.2.3.2; ISO 16684-1 8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `by` | enum | `pdfa-2` | pdfa-1, pdfa-2, extension_schema |  |
| `scope` | enum | `document` | document, page, object, any | The document packet is what the archive and print standards regulate. |

### Conformance

Declared and verified conformance to PDF standards.

#### `conformance.claim`: Declared conformance

What the document claims to conform to, read from its metadata.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 19005-2 6.6.4; ISO 15930-7 6.5; ISO 24517-1 6.4; ISO 14289-1 5; ISO 16612-2 6.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `family` | enum | `pdfa` | pdfa, pdfx, pdfe, pdfua, pdfvt |  |
| `field` | enum | `label` | label, part, conformance, amendment, well_formed | The label (for example 2b) is the most useful single value. |

#### `conformance.meets`: Conforms to a standard

Runs Kura's full validator for the selected standard and reports each violation.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: ISO 19005; ISO 15930; ISO 24517-1; ISO 16612-2; ISO 14289-1

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `standard` | enum | `pdfa-2b` | pdfa-1a, pdfa-1b, pdfa-2a, pdfa-2b, pdfa-2u, pdfa-3a, pdfa-3b, pdfa-3u, pdfa-4, pdfa-4e, pdfa-4f, pdfx-1a, pdfx-3, pdfx-4, pdfx-4p, pdfx-5g, pdfx-5n, pdfx-5pg, pdfx-6, pdfx-6n, pdfx-6p, pdfe-1, pdfvt-1, pdfvt-2, pdfvt-3, pdfua-1 |  |
| `embedded_files` | bool | `false` |  | Validate embedded PDF files instead of the document itself. |

### Logical structure

Tags, artifacts, alternative text, headings, tables and lists.

#### `structure.alternate_text`: Alternative descriptions

Figures, formulas, links, annotations and form fields carry alternative text, and the text says something.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 14289-1 7.3; ISO 14289-1 7.7; ISO 14289-1 7.18; WCAG 2.2 1.1.1; Matterhorn 13-004; Matterhorn 28-005
- Notes: Values: figure_without_alt, figure_with_actual_text, formula_without_alt, annotation_without_alt, link_without_contents, form_field_without_alt (no TU tooltip and no Alt on its structure element), actual_text_private_use, placeholder_alt (an Alt that is blank, only punctuation, a file name such as photo.jpg, or one of the placeholder words).

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `placeholders` | list | `image, picture, figure, graphic, photo, img, untitled` |  | For placeholder_alt: alternative texts that count as placeholders. Trailing numbers are ignored, so 'Image 3' matches 'image'. Words that name the kind of content instead of describing it. |

#### `structure.artifact`: Artifact type

Type and subtype of an artifact.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: object
- References: ISO 32000-2 14.8.2.2; ISO 14289-1 7.8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `part` | enum | `type` | type, subtype, header_footer |  |

#### `structure.content_tagging`: Content is tagged or artifact

Page content is either part of the logical structure or marked as artifact.

- Value: `enum`
- Operators: `==`, `!=`, `in`, `not_in`
- Applies to: object
- References: ISO 14289-1 7.1; ISO 32000-2 14.8.2.2
- Notes: Values: structure, artifact, neither, artifact_inside_structure, structure_inside_artifact.

No parameters.

#### `structure.element`: Structure element

Properties of a structure element.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: structure element
- References: ISO 32000-2 14.7.2; ISO 14289-1 7.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `type` | type, child_count, language, has_language, language_valid, language_is_text_string |  |

#### `structure.empty_block`: Empty block element

A block-level structure element has no content.

- Value: `bool`
- Operators: `is`
- Applies to: structure element
- References: ISO 14289-1 7.1

No parameters.

#### `structure.heading_level`: Heading level

Highest heading level used, or the gap between heading level and table-of-contents nesting.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 14289-1 7.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `measure` | enum | `highest` | highest, toc_difference |  |

#### `structure.headings`: Heading structure

Heading levels and their sequence.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 14289-1 7.4
- Notes: Values: numbered, both_h_and_numbered, several_h_in_node, skipped_levels, adjacent_same_level, first_not_h1. Use structure.heading_level for numbers.

No parameters.

#### `structure.language_change`: Unmarked change of language

Text written in a script that the language in effect does not use, with no Lang entry on an enclosing structure element to announce the change.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: WCAG 2.2 3.1.2; ISO 14289-1 7.2
- Notes: Values: one script name per passage (arabic, cyrillic, greek, hebrew, han, kana, hangul, devanagari, thai). Passages are the text of one marked-content sequence, or one text run when untagged. Latin script is never reported, since names and terms in Latin letters are normal in every language. Documents without a language are left to structure.language_unknown.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `min_letters` | count | `3` |  | Letters of the foreign script in a row (spaces and punctuation between them allowed) a passage needs before it counts. Shorter passages are usually symbols, abbreviations or names. |

#### `structure.language_unknown`: Natural language cannot be determined

Text whose language cannot be determined from Lang entries anywhere up the hierarchy.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 14289-1 7.2; ISO 32000-2 14.9.2
- Notes: Values: page_content, alt_text, actual_text, expansion_text, annotation_contents, outline, form_field_tooltip, metadata.

No parameters.

#### `structure.list`: List structure

Counts and rules for list elements.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: structure element
- References: ISO 14289-1 7.6; ISO 32000-2 14.8.4.8.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `items` | items, labels_per_item, bodies_per_item, has_caption, caption_first, nested, standard_types_only, label_language, label_has_language, label_language_differs, label_language_set |  |

#### `structure.nesting`: Annotation and field nesting

Annotations, links, form fields and printer's marks sit in the right structure elements.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 14289-1 7.18; ISO 14289-1 7.18.5; ISO 32000-2 14.8.4.7; WCAG 2.2 1.3.1; Matterhorn 28-011
- Notes: Values: annotation_not_in_annot, link_not_in_link (no OBJR from a Link element), link_without_struct_parent, field_not_in_form, printer_mark_in_structure, printer_mark_not_artifact.

No parameters.

#### `structure.notes`: Note identifiers

Note elements carry unique ID entries.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 14289-1 7.9
- Notes: Values: missing_id, duplicate_id.

No parameters.

#### `structure.standard_type`: Uses standard structure types

Element types are standard types, directly or through the role map.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 14.8.4; ISO 14289-1 7.1
- Notes: Values: non_standard_type, standard_remapped_to_standard, standard_remapped_to_non_standard, circular_role_map.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `standard` | enum | `pdf-1.7` | pdf-1.7, pdf-2.0 |  |

#### `structure.table`: Table structure

Counts and rules for table elements.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: structure element
- References: ISO 14289-1 7.5; ISO 32000-2 14.8.4.8.3; WCAG 2.2 1.3.1; Matterhorn 15-003
- Notes: regular: every row has the same number of cells, counting spans. has_summary: a Summary attribute or a Caption child. empty_header_cells: TH cells with no content, Alt or ActualText. th_without_scope_or_headers: TH cells without Scope in a table whose cells do not use Headers attributes instead (0 when Headers are used).

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `regular` | regular, th_cells, td_cells, thead, tbody, tfoot, has_caption, caption_misplaced, th_without_scope, nested, standard_types_only, thead_without_tbody, tfoot_without_tbody, has_summary, empty_header_cells, th_without_scope_or_headers |  |

#### `structure.toc`: Table of contents structure

Rules for TOC and TOCI elements.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 14.8.4.4
- Notes: Values: nested_toc, toci_direct_content, toci_child_not_label_or_reference.

No parameters.

#### `structure.tree`: Structure tree problems

Rule violations in the structure tree and its parent tree.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 32000-2 14.7; ISO 14289-1 7.1; ISO 19005-2 6.7
- Notes: Values: missing_root, element_without_type, inconsistent_parent_tree, missing_parent_tree_entry, marked_content_without_parent, dangling_marked_content_reference, marked_content_reference_without_page, page_key_wrong_type, object_reference_without_struct_parent, reference_to_object_without_marked_content, reference_to_object_without_parent_key, unreferenced_marked_content, unreferenced_element, invalid_class, malformed_class_array, duplicate_mcid, suspects_true.

No parameters.

### Output intents

Print conditions declared by the document.

#### `output_intent.colour_exchange`: Embedded colour exchange data

Spot colour measurement data (CxF/X-4) attached to the output intent and its consistency with the document's spot colours.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 17972-4; ISO 15930-9 6
- Notes: conformance is cxf-x-4, cxf-x-4a or cxf-x-4b.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `conformance` | conformance, has_entry_for_colour, colours_without_entry, process_colours_without_entry, stray_entries, in_data_and_mixing_hints, valid, mixing_hints_present |  |

#### `output_intent.count`: Number of output intents

How many output intents the document declares, optionally of one subtype.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: document
- References: ISO 32000-2 14.11.5; ISO 15930-7 6.2.3; ISO 19005-2 6.2.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `subtype` | enum | `pdfx` | pdfx, pdfa, pdfe, any | The PDF/X output intent is what print checks normally mean. |

#### `output_intent.entry`: Output intent entry

Value or presence of an entry of the output intent.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 32000-2 14.11.5; ISO 15930-7 6.2.3; ISO 19005-2 6.2.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `subtype` | enum | `pdfx` | pdfx, pdfa, pdfe, any | The PDF/X output intent is what print checks normally mean. |
| `entry` | enum | `identifier` | identifier, condition, registry, info, subtype, has_profile, has_registry, identifier_is_string, condition_is_string, registry_is_string, info_is_string, condition_is_standard, profile_ref_is_dictionary, profiles_same_in_all, device_independent_allowed, min_halftone_resolution, min_bitmap_resolution, max_plates, uses_nchannel_profile, alternate_colour_spaces, profile_component_mismatch, referenced_profiles, referenced_profile_available |  |

#### `output_intent.profile_reference`: Referenced output profile

Problems in the DestOutputProfileRef dictionary of a PDF/X-4p, 5n or 6p output intent.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 15930-7 6.2.3.3; ISO 15930-9 6
- Notes: Values: missing_name, name_not_string, missing_icc_version, icc_version_not_string, icc_version_bad, missing_checksum, checksum_not_16_bytes, missing_colour_space, colour_space_not_string, colour_space_not_profile_space, missing_urls, url_not_file_spec, url_without_file_system, missing_colorant_table, colorant_table_names, colorant_table_cmyk, colorant_table_black, missing_xclr_space.

No parameters.

### Variable data

Document parts and encapsulated graphics of variable-data documents.

#### `variable_data.document_parts`: Document part structure

Rule violations in the document part (DPart) hierarchy of a variable-data document.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: document
- References: ISO 16612-2 6.3; ISO 32000-2 14.12
- Notes: Values: missing_root, root_not_valid, root_missing_type, root_missing_node_names, root_missing_record_level, root_not_referenced, node_names_not_names, node_names_depth_mismatch, part_missing_type, part_missing_parent, part_missing_children, part_missing_start, part_missing_end, part_missing_metadata, part_several_parents, part_no_parent, part_parent_invalid, start_end_same_page_error, page_outside_parent_range, page_missing_dpart, page_order_differs, page_with_range_part, metadata_key_not_unique, metadata_key_not_nmtoken, metadata_managed_suspect_overlap, metadata_managed_not_dict, metadata_suspect_not_dict, metadata_has_managed, metadata_has_suspect, not_in_object_streams, all_in_object_streams, spread_over_object_streams.

No parameters.

#### `variable_data.encapsulated`: Encapsulated graphics

Reusable (encapsulated) XObjects and how they are used across records.

- Value: `list`
- Operators: `includes`, `excludes`, `count`
- Applies to: object
- References: ISO 16612-2 6.6
- Notes: Values: marked_encapsulated, marked_true, image_encapsulated, all_independent, inside_encapsulated, used_by_several_records, graphics_state_set_before_use.

No parameters.

#### `variable_data.part_value`: Document part values

Values from the document part hierarchy.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: document
- References: ISO 16612-2 6.3; ISO 16612-2 6.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `part_value` | enum | `depth` | depth, node_names, record_level, root_type, part_type, pages_referencing, metadata_value, version, has_version, last_modified, has_last_modified, last_modified_is_date, last_modified_consistent, environment_present, environment_is_text, scope_present, scope |  |
| `key` | text | empty |  | For metadata_value: the document part metadata key. |

#### `variable_data.xobject_uses`: XObject reuse count

How many times an XObject is drawn in the document.

- Value: `count`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: object
- References: ISO 16612-2 6.6

No parameters.

### Barcodes

Barcodes found on the rendered page and their print quality.

#### `barcode.dimension`: Barcode dimension

Width, height, module width, quiet zone or bar width reduction of a decoded barcode.

- Value: `length`
- Operators: `<`, `<=`, `==`, `!=`, `>=`, `>`, `multiple_of`
- Applies to: barcode
- References: ISO/IEC 15416 5.4; ISO/IEC 15420 5
- Notes: bar_width_reduction can be given as a length or as percent of the module width.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `measure` | enum | `width` | width, height, module_width, quiet_zone, bar_width_reduction |  |
| `unit` | enum | `mm` | pt, mm, cm, in, percent |  |

#### `barcode.found`: Barcode found

Kura decodes a barcode on the rendered page.

- Value: `bool`
- Operators: `is`
- Applies to: barcode
- References: ISO/IEC 15416; ISO/IEC 15415

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `resolution` | resolution | `300ppi` |  | 300 ppi resolves the narrowest bars of common retail symbols. |

#### `barcode.property`: Barcode property

A property of a decoded barcode.

- Value: `text`
- Operators: `==`, `!=`, `in`, `not_in`, `contains`, `starts_with`, `ends_with`, `matches`, `finds`, `occurs_in`
- Applies to: barcode
- References: ISO/IEC 15416; ISO/IEC 15415; ISO/IEC 15420

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `property` | enum | `symbology` | symbology, value, rotation, in_area, colour_contrast |  |

#### `barcode.quiet_zone_sufficient`: Quiet zone is large enough

The blank margin around the barcode meets the symbology's minimum.

- Value: `bool`
- Operators: `is`
- Applies to: barcode
- References: ISO/IEC 15416 5.4

No parameters.

### Scope helpers

Checks that select items unconditionally.

#### `scope.every`: Every item

Always true for each item of the chosen kind. Use it to attach a fix or a script to all pages, objects or annotations.

- Value: `bool`
- Operators: `is`
- Applies to: document
- References: Kura
- Notes: Matches every item in its scope; use it to apply a condition to all pages, objects or annotations.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `level` | enum | `document` | document, page, object, annotation |  |

## Fixes

### Document

#### `document.associate_files`: Associate embedded files

Adds the associated-files relationship for embedded files that lack it.

- References: ISO 32000-2 14.13; ISO 19005-3 6.8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `relationship` | enum | `Unspecified` | Source, Data, Alternative, Supplement, Unspecified |  |

#### `document.linearize`: Optimise for web

Writes the file linearized for fast web view.

- References: ISO 32000-2 Annex F

No parameters.

#### `document.remove_conformance_claim`: Remove a conformance claim

Removes the identification of a standard from the metadata.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `family` | enum | `pdfua` | pdfa, pdfx, pdfe, pdfua, pdfvt |  |

#### `document.set_info`: Set document information

Sets a document information entry and its XMP equivalent.

- References: ISO 32000-2 14.3.3; ISO 19005-2 6.6.2.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `key` | enum | `Title` | Title, Author, Subject, Keywords, Creator, Producer, CreationDate, ModDate, Trapped, Lang, custom |  |
| `name` | text | empty |  | For custom: the entry name. |
| `value` | text | required |  | Text, a {variable}, 'now' for dates, or 'file_name' for the title. |
| `only_if_empty` | bool | `false` |  |  |

#### `document.set_view`: Set opening view

Sets how the document opens: layout, panels, window options and viewer preferences.

- References: ISO 32000-2 7.7.2; ISO 32000-2 12.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `page_layout` | enum | empty | SinglePage, OneColumn, TwoColumnLeft, TwoColumnRight, TwoPageLeft, TwoPageRight |  |
| `page_mode` | enum | empty | UseNone, UseOutlines, UseThumbs, FullScreen, UseOC, UseAttachments |  |
| `show_title` | bool | empty |  |  |
| `fit_window` | bool | empty |  |  |
| `center_window` | bool | empty |  |  |
| `hide_toolbar` | bool | empty |  |  |
| `hide_menubar` | bool | empty |  |  |

### Page

#### `page.add_marks`: Add printer's marks

Adds crop and registration marks around the trim box, enlarging the media box if needed.

- References: ISO 32000-2 14.11.3

- Notes: Marks are drawn in the registration colour and tagged as printer's marks.

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `offset` | length | `3mm` |  | Marks start outside a 3 mm bleed (common print practice). |
| `length` | length | `5mm` |  | Kura default, matches common print practice. |
| `line_width` | length | `0.25pt` |  | Thin enough to be precise, thick enough to print. |
| `enlarge_media` | bool | `true` |  |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.crop_to`: Crop to content

Sets the crop box to the visible content or to everything that leaves ink.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `to` | enum | `content` | content, ink |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.mirror`: Mirror pages

Mirrors pages horizontally or vertically.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `axis` | enum | `horizontal` | horizontal, vertical |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.move_origin`: Move media box to origin

Shifts page content and boxes so the media box starts at 0 0.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.rasterize`: Rasterize page

Replaces the page content with an image of the rendered page, keeping the text invisible on top.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `resolution` | resolution | `300ppi` |  | 300 ppi is the usual print resolution for continuous-tone images. |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.remove_user_unit`: Remove page scaling

Removes UserUnit so pages print at their nominal size.

- References: ISO 32000-2 7.7.3.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.repair_resources`: Repair missing resources

Adds resources that content uses but that are missing from the page.

- References: ISO 32000-2 7.8.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.rotate`: Rotate pages

Turns pages clockwise by a multiple of 90 degrees.

- References: ISO 32000-2 7.7.3.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `angle` | angle | `90` | 90, 180, 270 |  |
| `only` | enum | `all` | all, portrait, landscape |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.scale`: Scale pages

Scales pages proportionally to a target size, optionally only when they are larger, and can scale the content within an unchanged page.

- References: ISO 216

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `width` | length | required |  |  |
| `height` | length | required |  |  |
| `mode` | enum | `page` | page, content_only |  |
| `only_if_larger` | bool | `false` |  |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `page.set_box`: Set a page box

Sets a page box from another box, from the visible content or from printer's marks, with margins.

- References: ISO 32000-2 14.11.2; ISO 15930-7 6.2

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `trim` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `from` | enum | `crop` | media, crop, bleed, trim, art, content, marks |  |
| `left` | length | `0mm` |  |  |
| `right` | length | `0mm` |  |  |
| `top` | length | `0mm` |  |  |
| `bottom` | length | `0mm` |  |  |
| `when` | enum | `always` | always, missing |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

### Page content

#### `content.bring_to_front`: Bring objects to front

Moves selected objects to the top of the painting order.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `objects` | text | required |  |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `content.fill_as_stroke`: Thin fills to strokes

Converts very thin filled shapes into strokes.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `max_width` | length | `0.5pt` |  |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `content.fix_negative_dash`: Repair negative dash values

Replaces negative dash lengths and phases.

- References: ISO 32000-2 8.4.3.6

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `content.remove_outside`: Remove content outside a box

Removes objects that lie completely outside a page box.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `box` | enum | `media` | media, crop, bleed, trim, art | The trim box is the finished size of the page (ISO 32000-2 14.11.2). |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `content.smooth_lines_to_curves`: Smooth polylines into curves

Replaces chains of short straight segments with curves.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

### Colour

#### `color.convert`: Convert colours

Converts colours to a destination profile with rules per object kind and source space.

- References: ISO 15076-1; ISO 15930-7 6.2.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `destination` | text | required |  | ICC profile name, file or 'output_intent'. |
| `rules` | list | required |  | Each rule: {objects, sources, action, intent}. objects: all, images, lossy_images, lossless_images, vector, text, shadings. sources: device_rgb, calibrated_rgb, device_cmyk, calibrated_cmyk, device_gray, calibrated_gray, lab, spot, registration. action: convert, keep, decalibrate, convert_alternate, decalibrate_and_convert, tag. |
| `embed` | enum | `output_intent` | output_intent, source, none |  |
| `preserve_black` | bool | `true` |  | Keeps black text and line art on the black plate (common print practice). |
| `black_point_compensation` | bool | `true` |  |  |
| `process_separations_as_cmyk` | bool | `false` |  |  |
| `grey_to_black` | bool | `false` |  |  |
| `rgb_grey_to_black_tolerance` | percent | empty |  |  |
| `device_link` | text | empty |  |  |
| `quick` | bool | `false` |  | Fast conversion without profiles (UCR/GCR formula). |
| `black_generation` | percent | empty |  |  |
| `undercolour_removal` | percent | empty |  |  |

#### `color.embed_measurements`: Embed spot colour measurements

Attaches spectral measurement data for spot colours to the output intent.

- References: ISO 17972-4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `source` | text | required |  |  |

#### `color.icc_to_device`: ICC to device colour

Replaces ICC-based colour spaces by the matching device space.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `color.icc_to_v2`: Downgrade ICC v4 profiles

Replaces ICC version 4 profiles by version 2 equivalents for older workflows.

- References: ISO 15930-1 6.2

No parameters.

#### `color.map`: Replace colours

Replaces one colour by another within a tolerance.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `from` | text | required |  | e.g. 'cmyk 0 100 100 0'. |
| `to` | text | required |  |  |
| `tolerance` | percent | `1%` |  |  |
| `objects` | enum | `all` | all, text, vector, images |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `color.nchannel_to_devicen`: NChannel to DeviceN

Converts NChannel colour spaces to plain DeviceN.

- References: Kura

No parameters.

#### `color.process_to_spot`: Process colour to spot

Turns a process colour combination into a named spot colour.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `from` | text | required |  |  |
| `spot` | text | required |  |  |
| `tolerance` | percent | `1%` |  |  |

#### `color.split_devicen`: Split DeviceN

Converts DeviceN colour to separate spot and process colours.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `process_to` | enum | `DeviceCMYK` | DeviceCMYK, keep |  |

#### `color.to_library_spot`: Match colours to a spot library

Replaces colours close to a library colour (within a colour difference) by that spot colour.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `library` | text | required |  |  |
| `max_delta_e` | number | `3` |  | ΔE 3 is a common just-noticeable difference in print. |

### Transparency

#### `transparency.flatten`: Flatten transparency

Replaces transparency with opaque vector or raster content that looks the same.

- References: ISO 15930-1 6.3; ISO 19005-1 6.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `raster_resolution` | resolution | `300ppi` |  |  |

#### `transparency.remove_unneeded_groups`: Remove unneeded transparency groups

Drops page groups that contain nothing transparent.

- References: ISO 32000-2 11.6.6

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `transparency.set_blend_space`: Set page blending space

Gives pages a transparency group with the chosen blending colour space.

- References: ISO 15930-7 6.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `space` | enum | `output_intent` | output_intent, CMYK, sRGB, Gray | The print condition's space is what PDF/X expects. |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

### Text

#### `text.add_invisible_copy`: Add searchable text layer

Adds an invisible copy of the text, for example over outlined or rasterized text.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `text.set_render_mode`: Set text render mode

Sets the render mode of all text, for example making invisible text visible.

- References: ISO 32000-2 9.3.6

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `mode` | enum | `fill` | fill, stroke, fill_and_stroke, invisible |  |
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

### Fonts

#### `font.rename`: Rename fonts

Changes the PostScript name of fonts.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `from` | text | required |  |  |
| `to` | text | required |  |  |

#### `font.to_outlines`: Convert text to outlines

Draws text as paths; the font is no longer needed.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only` | enum | `all` | all, truetype_to_cff |  |

### Images

#### `image.downsample`: Downsample images

Reduces image resolution above a threshold.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `kind` | enum | `colour_and_grey` | colour_and_grey, colour, grey, bitmap |  |
| `target` | resolution | `300ppi` |  |  |
| `above` | resolution | `450ppi` |  | 1.5 times the target avoids resampling images that are only slightly over. |

#### `image.expand_filter_names`: Expand abbreviated filter names

Replaces abbreviated filter names in image dictionaries by full names.

- References: ISO 32000-2 8.9.7

No parameters.

#### `image.merge_fragments`: Merge image fragments

Joins images that were cut into strips back into one image.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `only_where` | list | empty |  | Names of checks; the fix runs only on pages where one of them reported a hit. Empty: every page. |

#### `image.recompress`: Recompress images

Re-encodes JPEG 2000 images as JPEG.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `quality` | number | `90` |  |  |

#### `image.set_interpolation`: Image interpolation

Sets or removes the Interpolate flag.

- References: ISO 19005-2 6.2.8

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `value` | enum | `remove` | remove, on, off |  |

#### `image.sharpen`: Sharpen images

Applies an unsharp mask.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `radius` | number | `1` |  |  |
| `amount` | number | `0.5` |  |  |

#### `image.upsample`: Upsample low-resolution images

Raises images below a threshold to the target resolution.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `target` | resolution | `300ppi` |  |  |
| `below` | resolution | `300ppi` |  |  |

### Layers

#### `layer.flatten`: Flatten layers

Merges visible layers into the page and discards hidden ones.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `keep_hidden` | bool | `false` |  |  |

#### `layer.move_objects`: Move objects to a layer

Puts selected objects (or all page content) on a layer, also inside form XObjects; optionally marks the layer as a processing step.

- References: ISO 32000-2 8.11; ISO 19593-1 5

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `layer` | text | required |  |  |
| `objects` | text | empty |  | Check that selects the objects; empty: all content. |
| `processing_step` | enum | empty | Structural, Dimensions, Cutting, Creasing, Perforating, Folding, Varnish, White, Braille, Positions, Legend |  |

#### `layer.set`: Change layer properties

Renames layers or changes their state, matched by name or pattern.

- References: ISO 32000-2 8.11.4

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `match` | text | required |  |  |
| `name` | text | empty |  |  |
| `default_state` | enum | empty | on, off |  |
| `print` | enum | empty | always, never, when_visible |  |
| `export` | enum | empty | always, never, when_visible |  |
| `view` | enum | empty | always, never, when_visible |  |
| `locked` | bool | empty |  |  |
| `zoom_min` | number | empty |  |  |
| `zoom_max` | number | empty |  |  |
| `name_unnamed` | bool | `false` |  |  |
| `reset_to_default` | bool | `false` |  |  |

### Annotations

#### `annotation.handle`: Handle annotations

Removes, flattens or hides annotations by type.

- References: ISO 32000-2 12.5; ISO 19005-2 6.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `types` | list | required |  | Annotation subtypes, or 'multimedia', 'unknown', 'all'. |
| `action` | enum | `remove` | remove, flatten, hide, print |  |

### Forms

#### `form.generate_appearances`: Generate field appearances

Creates appearance streams for form fields that lack them.

- References: ISO 32000-2 12.7.4; ISO 19005-2 6.4

No parameters.

### Metadata

#### `metadata.uncompress_xmp`: Store XMP uncompressed

Rewrites metadata streams without compression so other tools can read them.

- References: ISO 19005-2 6.6.2.1

No parameters.

### Logical structure

#### `link.from_text`: Links from URLs in text

Adds link annotations over web addresses found in the text.

- References: Kura

No parameters.

#### `outline.from_headings`: Bookmarks from headings

Builds bookmarks from tagged headings or from text styles.

- References: Kura

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `source` | enum | `tags` | tags, text |  |

#### `outline.repair`: Repair bookmarks

Fixes broken bookmark links and outline counts.

- References: ISO 32000-2 12.3.3

No parameters.

#### `structure.actual_text_for_symbols`: Actual text for symbol fonts

Adds ActualText to text in symbolic fonts so it reads correctly.

- References: ISO 14289-1 7.21.7

No parameters.

#### `structure.mark_header_footer`: Mark headers and footers as artifacts

Detects running headers and footers and marks them as pagination artifacts.

- References: ISO 14289-1 7.8

No parameters.

#### `structure.resolve_role_map`: Resolve role map

Replaces custom element types by the standard types they map to.

- References: ISO 32000-2 14.8.4

No parameters.

### Output intents

#### `output_intent.remove_profile`: Remove output profile

Removes the embedded profile from output intents that identify a registered condition.

- References: Kura

No parameters.

#### `output_intent.set`: Set output intent

Sets the output intent profile and its identifiers, replacing or keeping existing intents; or adds page-level intents.

- References: ISO 32000-2 14.11.5; ISO 15930-7 6.2.3

| Parameter | Type | Default | Values | Notes |
|---|---|---|---|---|
| `profile` | text | required |  |  |
| `identifier` | text | empty |  |  |
| `condition` | text | empty |  |  |
| `registry` | text | `http://www.color.org` |  | The ICC registry is the standard registry for characterised printing conditions. |
| `keep_existing` | bool | `false` |  |  |
| `pages` | text | empty |  | Page range for page-level intents (PDF/X-6). |

## Output codes

| Code | Meaning |
|---|---|
| `CHECK_HIT` | A check found something. |
| `CHECK_NOT_EVALUATED` | A condition could not be evaluated for this file. |
| `PROFILE_INVALID` | The profile is not valid Kura JSON; the message names the field. |
| `PROFILE_EMPTY` | The profile has no checks to run in check mode. |
| `PROFILE_TARGET` | The profile's convert section chose the target standard. |
| `VARIABLE_SET` | A variable received a value. |
| `FIX_SCHEDULED` | A fix will run during conversion. |
| `FIX_DONE` | A fix changed the document. |
| `FIX_NOT_NEEDED` | A fix's only_where checks found nothing. |
| `FIX_UNKNOWN` | The fix id is not in the catalogue. |
| `ICC_PROFILE_MISSING` | A named ICC profile was not found. |
| `PLAN_STEP` | A plan step ran; the message gives its result and the next step. |
| `PLAN_STEP_FAILED` | A converting plan step failed. |
| `PLAN_NO_CONVERSION` | The plan ran no converting step; output equals input. |

Hit messages read: '<severity>: <check name> (<n> hit(s), pages <ranges>)' with the check's description when given.
