static const char* const kOperators[] = {"<", "<=", "==", "!=", ">=", ">", "multiple_of", "is", "in", "not_in", "contains", "starts_with", "ends_with", "matches", "finds", "occurs_in", "includes", "excludes", "count", "fits_within", "exceeds"};

static const char* const kValueTypes[] = {"bool", "number", "count", "length", "percent", "angle", "resolution", "ratio", "bytes", "version", "date", "text", "enum", "list", "size"};

static const ParamDef kParams_c_action_broken_destination[] = {
    {"from", "enum", "any", true, false, "any|link|bookmark"},
};

static const ParamDef kParams_c_action_destination[] = {
    {"from", "enum", "any", true, false, "any|link|bookmark|action"},
};

static const ParamDef kParams_c_action_present[] = {
    {"type", "enum", "any", true, false, "any|goto|goto_remote|goto_embedded|goto_3d_view|launch|thread|uri|sound|movie|hide|named|submit_form|reset_form|import_data|javascript|set_ocg_state|rendition|transition|rich_media"},
    {"on", "enum", "any", true, false, "any|document|page|annotation|form_field|outline"},
    {"trigger", "enum", "any", true, false, "any|activate|open|additional"},
};

static const ParamDef kParams_c_action_trigger[] = {
    {"on", "enum", "any", true, false, "any|document|page|annotation|form_field"},
};

static const ParamDef kParams_c_annotation_entry[] = {
    {"entry", "enum", "contents", true, false, "contents|name|opacity|has_opacity|blend_mode|has_blend_mode|colour|colour_is_rgb|interior_colour|interior_colour_is_rgb|has_appearance|tab_order|has_tab_order"},
};

static const ParamDef kParams_c_attachment_count[] = {
    {"attached_to", "enum", "any", true, false, "any|document|page|page_object|annotation|form_xobject|image|structure_element|marked_content|document_part"},
    {"associated", "enum", "any", true, false, "any|yes|no"},
};

static const ParamDef kParams_c_attachment_is_pdf[] = {
    {"conforms_to", "enum", "", false, false, "pdfa-1a|pdfa-1b|pdfa-2a|pdfa-2b|pdfa-2u|pdfa-3a|pdfa-3b|pdfa-3u|pdfa-4|pdfa-4e|pdfa-4f|pdfx-1a|pdfx-3|pdfx-4|pdfx-4p|pdfx-5g|pdfx-5n|pdfx-5pg|pdfx-6|pdfx-6n|pdfx-6p|pdfe-1|pdfvt-1|pdfvt-2|pdfvt-3|pdfua-1"},
};

static const ParamDef kParams_c_attachment_property[] = {
    {"property", "enum", "name", true, false, "name|extension|description|mime_type|relationship|subtype|stream_type"},
};

static const ParamDef kParams_c_barcode_dimension[] = {
    {"measure", "enum", "width", true, false, "width|height|module_width|quiet_zone|bar_width_reduction"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in|percent"},
};

static const ParamDef kParams_c_barcode_found[] = {
    {"resolution", "resolution", "300ppi", true, false, ""},
};

static const ParamDef kParams_c_barcode_property[] = {
    {"property", "enum", "symbology", true, false, "symbology|value|rotation|in_area|colour_contrast"},
};

static const ParamDef kParams_c_color_channel_spread[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
};

static const ParamDef kParams_c_color_component_count[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
    {"level", "enum", "direct", true, false, "direct|alternate"},
};

static const ParamDef kParams_c_color_devicen_colorants[] = {
    {"which", "enum", "all", true, false, "all|process|remapped"},
};

static const ParamDef kParams_c_color_ink[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
    {"ink", "enum", "black", true, false, "cyan|magenta|yellow|black|cmy_max|any_spot|max"},
};

static const ParamDef kParams_c_color_ink_mix[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
};

static const ParamDef kParams_c_color_inks_used[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
    {"kind", "enum", "any", true, false, "any|process|spot"},
};

static const ParamDef kParams_c_color_overprint[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
};

static const ParamDef kParams_c_color_rendering_intent[] = {
    {"explicit_only", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_color_space[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
    {"level", "enum", "direct", true, false, "direct|base|alternate|icc_alternate|process"},
    {"icc_components", "count", "", false, false, ""},
};

static const ParamDef kParams_c_color_spot[] = {
    {"property", "enum", "name", true, false, "name|kind|colour_system|name_encoding_valid|alternate_value"},
    {"component", "count", "1", true, false, ""},
};

static const ParamDef kParams_c_color_total_ink[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
    {"process_only", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_color_value[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
    {"component", "count", "", false, false, ""},
};

static const ParamDef kParams_c_conformance_claim[] = {
    {"family", "enum", "pdfa", true, false, "pdfa|pdfx|pdfe|pdfua|pdfvt"},
    {"field", "enum", "label", true, false, "label|part|conformance|amendment|well_formed"},
};

static const ParamDef kParams_c_conformance_meets[] = {
    {"standard", "enum", "pdfa-2b", true, false, "pdfa-1a|pdfa-1b|pdfa-2a|pdfa-2b|pdfa-2u|pdfa-3a|pdfa-3b|pdfa-3u|pdfa-4|pdfa-4e|pdfa-4f|pdfx-1a|pdfx-3|pdfx-4|pdfx-4p|pdfx-5g|pdfx-5n|pdfx-5pg|pdfx-6|pdfx-6n|pdfx-6p|pdfe-1|pdfvt-1|pdfvt-2|pdfvt-3|pdfua-1"},
    {"embedded_files", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_content_box_relation[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
    {"also", "enum", "", false, false, "media|crop|bleed|trim|art"},
    {"subject", "enum", "object", true, false, "object|annotation"},
};

static const ParamDef kParams_c_content_contrast_sufficient[] = {
    {"kind", "enum", "text", true, false, "text|graphics|any"},
    {"normal_text_ratio", "ratio", "4.5", true, false, ""},
    {"large_text_ratio", "ratio", "3", true, false, ""},
    {"graphics_ratio", "ratio", "3", true, false, ""},
    {"large_size", "length", "18pt", true, false, ""},
    {"large_bold_size", "length", "14pt", true, false, ""},
    {"resolution", "resolution", "96ppi", true, false, ""},
};

static const ParamDef kParams_c_content_dash_length[] = {
    {"part", "enum", "dash", true, false, "dash|gap"},
    {"measure", "enum", "average", true, false, "average|longest|shortest"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_content_distance_to_box[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
    {"side", "enum", "outside", true, false, "outside|inside"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_content_in_area[] = {
    {"x", "length", "", false, true, ""},
    {"y", "length", "", false, true, ""},
    {"width", "length", "0mm", true, false, ""},
    {"height", "length", "0mm", true, false, ""},
    {"origin", "enum", "media", true, false, "media|crop|bleed|trim|art|lower_left|center"},
    {"tolerance", "length", "0.5mm", true, false, ""},
};

static const ParamDef kParams_c_content_outside_share[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
};

static const ParamDef kParams_c_content_path_measure[] = {
    {"measure", "enum", "points", true, false, "points|subpaths|open_subpaths|closed_subpaths|straight_segments|curves|rectangles|largest_subpath_points|smallest_subpath_points|consecutive_points|single_point|page_points"},
};

static const ParamDef kParams_c_content_relation[] = {
    {"other", "text", "", false, false, ""},
    {"distance", "length", "0.5mm", true, false, ""},
    {"min_objects", "count", "1", true, false, ""},
};

static const ParamDef kParams_c_content_shading[] = {
    {"property", "enum", "type", true, false, "type|radial_without_radius|bbox_valid"},
};

static const ParamDef kParams_c_content_shape_relation[] = {
    {"shape", "text", "trim", true, false, ""},
    {"offset", "length", "0mm", true, false, ""},
    {"edge_band", "length", "2mm", true, false, ""},
};

static const ParamDef kParams_c_content_sharp_corners[] = {
    {"max_angle", "angle", "30", true, false, ""},
    {"min_radius", "length", "0mm", true, false, ""},
    {"scope", "enum", "object", true, false, "object|page"},
};

static const ParamDef kParams_c_content_size[] = {
    {"dimension", "enum", "area", true, false, "width|height|area|shorter|longer"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_content_stroke_geometry[] = {
    {"measure", "enum", "length", true, false, "length|angle|corner_radius"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_content_stroke_style[] = {
    {"property", "enum", "pattern", true, false, "cap|join|miter_limit|dash_array|dash_phase|pattern|stroke_adjustment"},
};

static const ParamDef kParams_c_content_stroke_width[] = {
    {"unit", "enum", "pt", true, false, "pt|mm|cm|in"},
    {"ignore_white", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_document_colour_spaces[] = {
    {"scope", "enum", "document", true, false, "document|page"},
};

static const ParamDef kParams_c_document_content_stream_size[] = {
    {"unit", "enum", "kb", true, false, "bytes|kb|mb"},
};

static const ParamDef kParams_c_document_extension_level[] = {
    {"prefix", "text", "", false, false, ""},
};

static const ParamDef kParams_c_document_file_size[] = {
    {"unit", "enum", "kb", true, false, "bytes|kb|mb"},
};

static const ParamDef kParams_c_document_has[] = {
    {"feature", "enum", "", false, true, "bookmarks|page_labels|thumbnails|article_threads|named_destinations|unused_destinations|form_fields|interactive_annotations|file_specifications|alternate_presentations|piece_info|xmp_metadata|non_plain_text_xmp|named_optional_content|optional_content|mark_info|needs_rendering|javascript|embedded_files_name_tree"},
};

static const ParamDef kParams_c_document_header_conforms[] = {
    {"standard", "enum", "pdfa-2b", true, false, "pdf-2.0|pdfa-1b|pdfa-2b|pdfe-1"},
};

static const ParamDef kParams_c_document_job_options[] = {
    {"compare_to", "text", "", false, false, ""},
};

static const ParamDef kParams_c_document_language[] = {
    {"test", "enum", "value", true, false, "value|present|valid_tag|is_text_string"},
};

static const ParamDef kParams_c_document_largest_stream[] = {
    {"unit", "enum", "kb", true, false, "bytes|kb|mb"},
};

static const ParamDef kParams_c_document_open_view[] = {
    {"setting", "enum", "page_layout", true, false, "page_layout|page_mode|open_page|zoom|show_title|viewer_preferences|print_scaling|clip_to"},
};

static const ParamDef kParams_c_document_pages_uniform[] = {
    {"aspect", "enum", "size_and_orientation", true, false, "size|orientation|size_and_orientation"},
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
    {"tolerance", "length", "1pt", true, false, ""},
};

static const ParamDef kParams_c_document_pdf_version[] = {
    {"source", "enum", "effective", true, false, "effective|header|catalog"},
};

static const ParamDef kParams_c_document_requirements[] = {
    {"pdf2_only", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_document_script[] = {
    {"script", "text", "", false, true, ""},
};

static const ParamDef kParams_c_document_uses_object_streams[] = {
    {"except_structure", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_font_metric[] = {
    {"metric", "enum", "x_height", true, false, "x_height|stem_h|avg_width|max_width|bbox_left|bbox_bottom|bbox_right|bbox_top|largest_cid"},
};

static const ParamDef kParams_c_font_name[] = {
    {"source", "enum", "pdf", true, false, "pdf|embedded_program"},
};

static const ParamDef kParams_c_font_problem[] = {
    {"strict", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_form_field_name[] = {
    {"which", "enum", "full", true, false, "full|partial|alternate|export"},
};

static const ParamDef kParams_c_icc_header[] = {
    {"field", "enum", "version", true, false, "version|colour_space|device_class|pcs|rendering_intent|size|cmm|creator|manufacturer|model|platform|flags|attributes|date|profile_id|illuminant_x|illuminant_y|illuminant_z|signature_valid"},
    {"profile", "enum", "object", true, false, "object|output_intent|output_intent_pdfa|output_intent_pdfe"},
};

static const ParamDef kParams_c_icc_matches_output_intent[] = {
    {"intent", "enum", "pdfx", true, false, "pdfx|pdfa|pdfe|destination"},
    {"field", "enum", "profile", true, false, "profile|colour_space|device_class"},
};

static const ParamDef kParams_c_icc_max_ink[] = {
    {"profile", "enum", "object", true, false, "object|output_intent|output_intent_pdfa|output_intent_pdfe"},
};

static const ParamDef kParams_c_icc_tag[] = {
    {"tag", "enum", "description", true, false, "description|copyright|calibration_date|device_manufacturer|device_model|technology|viewing_conditions|screening|rendering_info|named_colours"},
    {"profile", "enum", "object", true, false, "object|output_intent|output_intent_pdfa|output_intent_pdfe"},
};

static const ParamDef kParams_c_image_jpeg2000[] = {
    {"property", "enum", "channels", true, false, "channels|colour_specs|colour_method|approximate_specs|same_bit_depth|jab_colour_space"},
};

static const ParamDef kParams_c_image_opi[] = {
    {"version", "enum", "2.0", true, false, "1.3|2.0"},
    {"field", "enum", "file", true, false, "file|id|comments|type|colour|colour_c|colour_m|colour_y|colour_k|colour_name|crop_left|crop_top|crop_right|crop_bottom|size_width|size_height|position_llx|position_lly|position_lrx|position_lry|position_ulx|position_uly|position_urx|position_ury|resolution_x|resolution_y|tint|overprint|transparency|bits|samples|included_width|included_height|included_quality|main_image"},
};

static const ParamDef kParams_c_image_property[] = {
    {"property", "enum", "bits_per_component", true, false, "bits_per_component|width|height|filter|lossy|compressed|ascii_encoded|interpolate|interpolate_present|intent_present|is_xobject|stencil_mask|colour_key_mask|explicit_mask|soft_mask|soft_mask_in_data|soft_mask_in_data_value|soft_mask_matte|length_correct|invalid|bytes_compressed|bytes_uncompressed|compression_ratio|inline_size|jpeg_quality|opi_info|colour_type"},
    {"alternate", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_image_resolution[] = {
    {"axis", "enum", "lowest", true, false, "lowest|highest|horizontal|vertical"},
    {"kind", "enum", "any", true, false, "any|colour_or_grey|bitmap"},
};

static const ParamDef kParams_c_image_rotation[] = {
    {"right_angles_only", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_layer_configuration_name[] = {
    {"which", "enum", "configuration", true, false, "configuration|order"},
};

static const ParamDef kParams_c_layer_property[] = {
    {"property", "enum", "name", true, false, "name|has_name|creator|has_creator_info|locked|empty|in_navigation|radio_group|exported|printed|zoom_min|zoom_max|hidden_at_some_zoom|language|for_language|users|user_type|for_users|content_type|pagination_artifact|pagination_subtype|has_usage_user|metadata|listed"},
    {"scope", "enum", "object", true, false, "object|document"},
};

static const ParamDef kParams_c_layer_visible[] = {
    {"in", "enum", "default", true, false, "default|any_configuration|current"},
};

static const ParamDef kParams_c_metadata_info[] = {
    {"key", "text", "Title", true, true, ""},
};

static const ParamDef kParams_c_metadata_info_matches_xmp[] = {
    {"key", "enum", "all", true, false, "all|Title|Author|Subject|Keywords|Creator|Producer|CreationDate|ModDate|Trapped|GTS_PDFXVersion|ISO_PDFEVersion"},
};

static const ParamDef kParams_c_metadata_object_has_xmp[] = {
    {"object", "enum", "any", true, false, "any|page|image|font|form_xobject|icc_profile|property_list"},
};

static const ParamDef kParams_c_metadata_xmp_extension_schema[] = {
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_history_incomplete[] = {
    {"field", "enum", "any", true, false, "any|action|instanceID|parameters|softwareAgent|when"},
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_identification[] = {
    {"standard", "enum", "pdfa", true, false, "pdfa|pdfua|pdfx|pdfe|pdfvt"},
};

static const ParamDef kParams_c_metadata_xmp_namespaces[] = {
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_prefix[] = {
    {"namespace", "text", "http://purl.org/dc/elements/1.1/", true, true, ""},
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_problem[] = {
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_property[] = {
    {"name", "text", "", false, true, ""},
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_property_known[] = {
    {"by", "enum", "pdfa-2", true, false, "pdfa-1|pdfa-2|pdfx-4|pdfe-1|xmp-2004|xmp-2005|extension_schema"},
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_schema_file[] = {
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_metadata_xmp_size[] = {
    {"scope", "enum", "document", true, false, "document|page|object|any"},
    {"unit", "enum", "kb", true, false, "bytes|kb|mb"},
};

static const ParamDef kParams_c_metadata_xmp_value_type[] = {
    {"by", "enum", "pdfa-2", true, false, "pdfa-1|pdfa-2|extension_schema"},
    {"scope", "enum", "document", true, false, "document|page|object|any"},
};

static const ParamDef kParams_c_output_intent_colour_exchange[] = {
    {"property", "enum", "conformance", true, false, "conformance|has_entry_for_colour|colours_without_entry|process_colours_without_entry|stray_entries|in_data_and_mixing_hints|valid|mixing_hints_present"},
};

static const ParamDef kParams_c_output_intent_count[] = {
    {"subtype", "enum", "pdfx", true, false, "pdfx|pdfa|pdfe|any"},
};

static const ParamDef kParams_c_output_intent_entry[] = {
    {"subtype", "enum", "pdfx", true, false, "pdfx|pdfa|pdfe|any"},
    {"entry", "enum", "identifier", true, false, "identifier|condition|registry|info|subtype|has_profile|has_registry|identifier_is_string|condition_is_string|registry_is_string|info_is_string|condition_is_standard|profile_ref_is_dictionary|profiles_same_in_all|device_independent_allowed|min_halftone_resolution|min_bitmap_resolution|max_plates|uses_nchannel_profile|alternate_colour_spaces|profile_component_mismatch|referenced_profiles|referenced_profile_available"},
};

static const ParamDef kParams_c_page_aspect_ratio[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
};

static const ParamDef kParams_c_page_box_offset[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
    {"axis", "enum", "horizontal", true, false, "horizontal|vertical"},
    {"from", "enum", "media", true, false, "media|origin"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_page_box_present[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
};

static const ParamDef kParams_c_page_box_relation[] = {
    {"box", "enum", "crop", true, false, "media|crop|bleed|trim|art"},
    {"other", "enum", "media", true, false, "media|crop|bleed|trim|art"},
};

static const ParamDef kParams_c_page_box_size[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
    {"dimension", "enum", "width", true, false, "width|height|longer|shorter"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
    {"raw", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_page_content_stream[] = {
    {"property", "enum", "compression", true, false, "compression|size_kb|ascii_encoded"},
};

static const ParamDef kParams_c_page_empty[] = {
    {"within", "enum", "page", true, false, "page|trim|bleed"},
};

static const ParamDef kParams_c_page_in_range[] = {
    {"pages", "text", "", false, true, ""},
};

static const ParamDef kParams_c_page_label[] = {
    {"part", "enum", "label", true, false, "label|style|prefix"},
};

static const ParamDef kParams_c_page_orientation[] = {
    {"box", "enum", "media", true, false, "media|crop|bleed|trim|art"},
};

static const ParamDef kParams_c_page_output_intent[] = {
    {"subtype", "enum", "pdfx", true, false, "pdfx|pdfa|any"},
};

static const ParamDef kParams_c_page_size[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
    {"width", "length", "", false, false, ""},
    {"height", "length", "", false, false, ""},
    {"tolerance", "length", "1pt", true, false, ""},
    {"either_orientation", "bool", "true", true, false, ""},
};

static const ParamDef kParams_c_print_bleed[] = {
    {"side", "enum", "smallest", true, false, "smallest|left|right|top|bottom"},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_print_bleed_missing[] = {
    {"min", "length", "3mm", true, false, ""},
    {"shape", "enum", "trim", true, false, "trim|die_line"},
    {"die_line_check", "text", "", false, false, ""},
};

static const ParamDef kParams_c_print_empty_area[] = {
    {"width", "length", "", false, true, ""},
    {"height", "length", "", false, true, ""},
    {"margin", "length", "0mm", true, false, ""},
};

static const ParamDef kParams_c_print_halftone[] = {
    {"property", "enum", "present", true, false, "present|type|frequency|angle|spot_function|accurate_screens|name|origin|phase"},
    {"colorant", "enum", "any", true, false, "any|cyan|magenta|yellow|black"},
};

static const ParamDef kParams_c_print_images_on_cmy_only[] = {
    {"scope", "enum", "page", true, false, "page|document"},
};

static const ParamDef kParams_c_print_ink_area[] = {
    {"ink", "percent", "300%", true, true, ""},
    {"inks", "enum", "all", true, false, "all|process|black|spot"},
    {"sample_resolution", "resolution", "100ppi", true, false, ""},
    {"unit", "enum", "mm", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_print_ink_coverage[] = {
    {"min_area", "length", "1mm", true, false, ""},
    {"sample_resolution", "resolution", "100ppi", true, false, ""},
    {"area", "text", "", false, false, ""},
    {"box", "enum", "media", true, false, "media|crop|bleed|trim|art"},
};

static const ParamDef kParams_c_print_plates[] = {
    {"scope", "enum", "page", true, false, "page|document"},
    {"kind", "enum", "all", true, false, "all|process|spot"},
    {"non_empty", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_print_spot_naming[] = {
    {"scope", "enum", "document", true, false, "page|document"},
};

static const ParamDef kParams_c_print_trapped[] = {
    {"source", "enum", "any", true, false, "any|info|xmp"},
};

static const ParamDef kParams_c_print_used_area[] = {
    {"tolerance", "length", "1pt", true, false, ""},
};

static const ParamDef kParams_c_print_uses_plates[] = {
    {"plates", "list", "", false, true, ""},
    {"min_ink", "percent", "0%", true, false, ""},
    {"ignore_white", "bool", "true", true, false, ""},
};

static const ParamDef kParams_c_scope_every[] = {
    {"level", "enum", "document", true, false, "document|page|object|annotation"},
};

static const ParamDef kParams_c_seal_record[] = {
    {"field", "enum", "profile_name", true, false, "result|result_text|profile_name|profile_author|profile_author_version|profile_digest|profile_format"},
};

static const ParamDef kParams_c_security_encryption_dictionary[] = {
    {"entry", "enum", "version", true, false, "version|has_version|has_permissions|direct_strings|direct_arrays|direct_dictionaries|crypt_filter_direct_strings|crypt_filter_direct_arrays|crypt_filter_direct_dictionaries|crypt_filter_decode_name"},
};

static const ParamDef kParams_c_security_permission[] = {
    {"action", "enum", "copy_text", true, false, "print|modify|copy_text|annotate|fill_forms|accessibility|assemble|print_high"},
};

static const ParamDef kParams_c_signature_dictionary[] = {
    {"entry", "enum", "sub_filter", true, false, "sub_filter|byte_range|covers_whole_file|has_version|digest_location|digest_method|digest_value|transform_method|transform_permissions"},
};

static const ParamDef kParams_c_signature_present[] = {
    {"signed_only", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_structure_alternate_text[] = {
    {"placeholders", "list", "image\npicture\nfigure\ngraphic\nphoto\nimg\nuntitled", true, false, ""},
};

static const ParamDef kParams_c_structure_artifact[] = {
    {"part", "enum", "type", true, false, "type|subtype|header_footer"},
};

static const ParamDef kParams_c_structure_element[] = {
    {"property", "enum", "type", true, false, "type|child_count|language|has_language|language_valid|language_is_text_string"},
};

static const ParamDef kParams_c_structure_heading_level[] = {
    {"measure", "enum", "highest", true, false, "highest|toc_difference"},
};

static const ParamDef kParams_c_structure_language_change[] = {
    {"min_letters", "count", "3", true, false, ""},
};

static const ParamDef kParams_c_structure_list[] = {
    {"property", "enum", "items", true, false, "items|labels_per_item|bodies_per_item|has_caption|caption_first|nested|standard_types_only|label_language|label_has_language|label_language_differs|label_language_set"},
};

static const ParamDef kParams_c_structure_standard_type[] = {
    {"standard", "enum", "pdf-1.7", true, false, "pdf-1.7|pdf-2.0"},
};

static const ParamDef kParams_c_structure_table[] = {
    {"property", "enum", "regular", true, false, "regular|th_cells|td_cells|thead|tbody|tfoot|has_caption|caption_misplaced|th_without_scope|nested|standard_types_only|thead_without_tbody|tfoot_without_tbody|has_summary|empty_header_cells|th_without_scope_or_headers"},
};

static const ParamDef kParams_c_syntax_dictionary_error[] = {
    {"object_type", "text", "", false, false, ""},
};

static const ParamDef kParams_c_syntax_limit[] = {
    {"measure", "enum", "", false, true, "largest_integer|smallest_integer|largest_real|smallest_real|real_closest_to_zero_positive|real_closest_to_zero_negative|longest_string|longest_content_string|longest_name|largest_array|largest_dictionary|deepest_graphics_state_nesting"},
};

static const ParamDef kParams_c_syntax_unknown_operator[] = {
    {"version", "enum", "1.3", true, false, "1.0|1.1|1.2|1.3"},
};

static const ParamDef kParams_c_text_geometry[] = {
    {"measure", "enum", "rotation", true, false, "rotation|slant"},
};

static const ParamDef kParams_c_text_size[] = {
    {"measure", "enum", "font_size", true, false, "font_size|x_height|x_height_unscaled|x_height_percent"},
    {"unit", "enum", "pt", true, false, "pt|mm|cm|in"},
};

static const ParamDef kParams_c_text_state[] = {
    {"parameter", "enum", "render_mode", true, false, "render_mode|character_spacing|word_spacing|horizontal_scaling|leading|rise"},
};

static const ParamDef kParams_c_transparency_alpha[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
};

static const ParamDef kParams_c_transparency_blend_mode[] = {
    {"inherited", "bool", "false", true, false, ""},
};

static const ParamDef kParams_c_transparency_group_colour_space[] = {
    {"which", "enum", "nearest", true, false, "nearest|outermost|base|luminosity_mask"},
};

static const ParamDef kParams_c_transparency_used[] = {
    {"paint", "enum", "any", true, false, "any|fill|stroke"},
    {"how", "enum", "any", true, false, "any|soft_mask|alpha|blend_mode|image_soft_mask"},
};

static const ParamDef kParams_c_variable_data_part_value[] = {
    {"part_value", "enum", "depth", true, false, "depth|node_names|record_level|root_type|part_type|pages_referencing|metadata_value|version|has_version|last_modified|has_last_modified|last_modified_is_date|last_modified_consistent|environment_present|environment_is_text|scope_present|scope"},
    {"key", "text", "", false, false, ""},
};

static const ParamDef kParams_f_annotation_handle[] = {
    {"types", "list", "", false, true, ""},
    {"action", "enum", "remove", true, false, "remove|flatten|hide|print"},
};

static const ParamDef kParams_f_bleed_around_shape[] = {
    {"amount", "length", "3mm", true, false, ""},
    {"resolution", "resolution", "300ppi", true, false, ""},
    {"die_line_check", "text", "", false, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_bleed_generate[] = {
    {"amount", "length", "3mm", true, false, ""},
    {"method", "enum", "mirror", true, false, "mirror|stretch"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_color_convert[] = {
    {"destination", "text", "", false, true, ""},
    {"rules", "list", "", false, true, ""},
    {"embed", "enum", "output_intent", true, false, "output_intent|source|none"},
    {"preserve_black", "bool", "true", true, false, ""},
    {"black_point_compensation", "bool", "true", true, false, ""},
    {"process_separations_as_cmyk", "bool", "false", true, false, ""},
    {"grey_to_black", "bool", "false", true, false, ""},
    {"rgb_grey_to_black_tolerance", "percent", "", false, false, ""},
    {"device_link", "text", "", false, false, ""},
    {"quick", "bool", "false", true, false, ""},
    {"black_generation", "percent", "", false, false, ""},
    {"undercolour_removal", "percent", "", false, false, ""},
};

static const ParamDef kParams_f_color_embed_measurements[] = {
    {"source", "text", "", false, true, ""},
};

static const ParamDef kParams_f_color_icc_to_device[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_color_map[] = {
    {"from", "text", "", false, true, ""},
    {"to", "text", "", false, true, ""},
    {"tolerance", "percent", "1%", true, false, ""},
    {"objects", "enum", "all", true, false, "all|text|vector|images"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_color_process_to_spot[] = {
    {"from", "text", "", false, true, ""},
    {"spot", "text", "", false, true, ""},
    {"tolerance", "percent", "1%", true, false, ""},
};

static const ParamDef kParams_f_color_split_devicen[] = {
    {"process_to", "enum", "DeviceCMYK", true, false, "DeviceCMYK|keep"},
};

static const ParamDef kParams_f_color_to_library_spot[] = {
    {"library", "text", "", false, true, ""},
    {"max_delta_e", "number", "3", true, false, ""},
};

static const ParamDef kParams_f_content_bring_to_front[] = {
    {"objects", "text", "", false, true, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_content_fill_as_stroke[] = {
    {"max_width", "length", "0.5pt", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_content_fix_negative_dash[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_content_remove_outside[] = {
    {"box", "enum", "media", true, false, "media|crop|bleed|trim|art"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_content_smooth_lines_to_curves[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_document_associate_files[] = {
    {"relationship", "enum", "Unspecified", true, false, "Source|Data|Alternative|Supplement|Unspecified"},
};

static const ParamDef kParams_f_document_remove_conformance_claim[] = {
    {"family", "enum", "pdfua", true, false, "pdfa|pdfx|pdfe|pdfua|pdfvt"},
};

static const ParamDef kParams_f_document_set_info[] = {
    {"key", "enum", "Title", true, false, "Title|Author|Subject|Keywords|Creator|Producer|CreationDate|ModDate|Trapped|Lang|custom"},
    {"name", "text", "", false, false, ""},
    {"value", "text", "", false, true, ""},
    {"only_if_empty", "bool", "false", true, false, ""},
};

static const ParamDef kParams_f_document_set_view[] = {
    {"page_layout", "enum", "", false, false, "SinglePage|OneColumn|TwoColumnLeft|TwoColumnRight|TwoPageLeft|TwoPageRight"},
    {"page_mode", "enum", "", false, false, "UseNone|UseOutlines|UseThumbs|FullScreen|UseOC|UseAttachments"},
    {"show_title", "bool", "", false, false, ""},
    {"fit_window", "bool", "", false, false, ""},
    {"center_window", "bool", "", false, false, ""},
    {"hide_toolbar", "bool", "", false, false, ""},
    {"hide_menubar", "bool", "", false, false, ""},
};

static const ParamDef kParams_f_font_rename[] = {
    {"from", "text", "", false, true, ""},
    {"to", "text", "", false, true, ""},
};

static const ParamDef kParams_f_font_to_outlines[] = {
    {"only", "enum", "all", true, false, "all|truetype_to_cff"},
};

static const ParamDef kParams_f_graphics_remove[] = {
    {"settings", "list", "", false, true, "flatness|smoothness|transfer|black_generation|undercolour_removal|rendering_intent"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_graphics_set[] = {
    {"flatness", "number", "", false, false, ""},
    {"rendering_intent", "enum", "", false, false, "Perceptual|RelativeColorimetric|Saturation|AbsoluteColorimetric"},
    {"overprint_mode", "enum", "", false, false, "0|1"},
    {"fill_blend_mode", "text", "", false, false, ""},
    {"stroke_blend_mode", "text", "", false, false, ""},
    {"fill_alpha", "number", "", false, false, ""},
    {"stroke_alpha", "number", "", false, false, ""},
    {"black_point_compensation", "enum", "", false, false, "ON|OFF|Default"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_image_downsample[] = {
    {"kind", "enum", "colour_and_grey", true, false, "colour_and_grey|colour|grey|bitmap"},
    {"target", "resolution", "300ppi", true, false, ""},
    {"above", "resolution", "450ppi", true, false, ""},
};

static const ParamDef kParams_f_image_merge_fragments[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_image_recompress[] = {
    {"quality", "number", "90", true, false, ""},
};

static const ParamDef kParams_f_image_set_interpolation[] = {
    {"value", "enum", "remove", true, false, "remove|on|off"},
};

static const ParamDef kParams_f_image_sharpen[] = {
    {"radius", "number", "1", true, false, ""},
    {"amount", "number", "0.5", true, false, ""},
};

static const ParamDef kParams_f_image_upsample[] = {
    {"target", "resolution", "300ppi", true, false, ""},
    {"below", "resolution", "300ppi", true, false, ""},
};

static const ParamDef kParams_f_ink_dot_gain[] = {
    {"curve", "text", "", false, true, ""},
};

static const ParamDef kParams_f_ink_knockout_white[] = {
    {"objects", "enum", "all", true, false, "all|text|vector"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_ink_min_line_width[] = {
    {"width", "length", "0.25pt", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_ink_overprint_black[] = {
    {"objects", "enum", "all", true, false, "all|text|vector"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_ink_white_plate[] = {
    {"name", "text", "White", true, false, ""},
    {"look", "text", "cmyk 0 0 0 20", true, false, ""},
    {"mode", "enum", "objects", true, false, "objects|shape|background"},
    {"objects", "text", "", false, false, ""},
};

static const ParamDef kParams_f_layer_add_view[] = {
    {"name", "text", "", false, true, ""},
    {"hide", "text", "", false, false, ""},
    {"show", "text", "", false, false, ""},
    {"creator", "text", "", false, false, ""},
};

static const ParamDef kParams_f_layer_flatten[] = {
    {"keep_hidden", "bool", "false", true, false, ""},
};

static const ParamDef kParams_f_layer_move_objects[] = {
    {"layer", "text", "", false, true, ""},
    {"objects", "text", "", false, false, ""},
    {"processing_step", "text", "", false, false, ""},
};

static const ParamDef kParams_f_layer_set[] = {
    {"match", "text", "", false, true, ""},
    {"name", "text", "", false, false, ""},
    {"default_state", "enum", "", false, false, "on|off"},
    {"print", "enum", "", false, false, "always|never|when_visible"},
    {"export", "enum", "", false, false, "always|never|when_visible"},
    {"view", "enum", "", false, false, "always|never|when_visible"},
    {"locked", "bool", "", false, false, ""},
    {"zoom_min", "number", "", false, false, ""},
    {"zoom_max", "number", "", false, false, ""},
    {"name_unnamed", "bool", "false", true, false, ""},
    {"reset_to_default", "bool", "false", true, false, ""},
};

static const ParamDef kParams_f_outline_from_headings[] = {
    {"source", "enum", "tags", true, false, "tags|text"},
};

static const ParamDef kParams_f_output_intent_set[] = {
    {"profile", "text", "", false, true, ""},
    {"identifier", "text", "", false, false, ""},
    {"condition", "text", "", false, false, ""},
    {"registry", "text", "http://www.color.org", true, false, ""},
    {"keep_existing", "bool", "false", true, false, ""},
    {"pages", "text", "", false, false, ""},
};

static const ParamDef kParams_f_page_add_marks[] = {
    {"offset", "length", "3mm", true, false, ""},
    {"length", "length", "5mm", true, false, ""},
    {"line_width", "length", "0.25pt", true, false, ""},
    {"enlarge_media", "bool", "true", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_crop_to[] = {
    {"to", "enum", "content", true, false, "content|ink"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_mirror[] = {
    {"axis", "enum", "horizontal", true, false, "horizontal|vertical"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_move_origin[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_rasterize[] = {
    {"resolution", "resolution", "300ppi", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_remove_user_unit[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_repair_resources[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_rotate[] = {
    {"angle", "angle", "90", true, false, "90|180|270"},
    {"only", "enum", "all", true, false, "all|portrait|landscape"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_scale[] = {
    {"width", "length", "", false, true, ""},
    {"height", "length", "", false, true, ""},
    {"mode", "enum", "page", true, false, "page|content_only"},
    {"only_if_larger", "bool", "false", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_page_set_box[] = {
    {"box", "enum", "trim", true, false, "media|crop|bleed|trim|art"},
    {"from", "enum", "crop", true, false, "media|crop|bleed|trim|art|content|marks"},
    {"left", "length", "0mm", true, false, ""},
    {"right", "length", "0mm", true, false, ""},
    {"top", "length", "0mm", true, false, ""},
    {"bottom", "length", "0mm", true, false, ""},
    {"when", "enum", "always", true, false, "always|missing"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_place_barcode[] = {
    {"symbology", "enum", "code128", true, false, "ean13|ean8|upca|code128|code39|qr|datamatrix|pdf417"},
    {"data", "text", "", false, true, ""},
    {"width", "length", "", false, true, ""},
    {"height", "length", "", false, true, ""},
    {"color", "text", "cmyk 0 0 0 100", true, false, ""},
    {"x", "length", "0mm", true, false, ""},
    {"y", "length", "0mm", true, false, ""},
    {"anchor", "enum", "center", true, false, "lower_left|lower_right|upper_left|upper_right|center|left|right|top|bottom"},
    {"relative_to", "enum", "crop", true, false, "media|crop|bleed|trim|art"},
    {"rotation", "angle", "0", true, false, ""},
    {"layer", "text", "", false, false, ""},
    {"position", "enum", "front", true, false, "front|back"},
    {"pages", "text", "all", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_place_circle[] = {
    {"diameter", "length", "", false, true, ""},
    {"color", "text", "cmyk 0 0 0 100", true, false, ""},
    {"paint", "enum", "fill", true, false, "fill|stroke"},
    {"x", "length", "0mm", true, false, ""},
    {"y", "length", "0mm", true, false, ""},
    {"anchor", "enum", "center", true, false, "lower_left|lower_right|upper_left|upper_right|center|left|right|top|bottom"},
    {"relative_to", "enum", "crop", true, false, "media|crop|bleed|trim|art"},
    {"rotation", "angle", "0", true, false, ""},
    {"layer", "text", "", false, false, ""},
    {"position", "enum", "front", true, false, "front|back"},
    {"pages", "text", "all", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_place_html[] = {
    {"template", "text", "", false, true, ""},
    {"per_page", "bool", "true", true, false, ""},
    {"x", "length", "0mm", true, false, ""},
    {"y", "length", "0mm", true, false, ""},
    {"anchor", "enum", "center", true, false, "lower_left|lower_right|upper_left|upper_right|center|left|right|top|bottom"},
    {"relative_to", "enum", "crop", true, false, "media|crop|bleed|trim|art"},
    {"rotation", "angle", "0", true, false, ""},
    {"layer", "text", "", false, false, ""},
    {"position", "enum", "front", true, false, "front|back"},
    {"pages", "text", "all", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_place_pdf[] = {
    {"file", "text", "", false, true, ""},
    {"page", "count", "1", true, false, ""},
    {"scale", "number", "1", true, false, ""},
    {"x", "length", "0mm", true, false, ""},
    {"y", "length", "0mm", true, false, ""},
    {"anchor", "enum", "center", true, false, "lower_left|lower_right|upper_left|upper_right|center|left|right|top|bottom"},
    {"relative_to", "enum", "crop", true, false, "media|crop|bleed|trim|art"},
    {"rotation", "angle", "0", true, false, ""},
    {"layer", "text", "", false, false, ""},
    {"position", "enum", "front", true, false, "front|back"},
    {"pages", "text", "all", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_place_rectangle[] = {
    {"width", "length", "", false, true, ""},
    {"height", "length", "", false, true, ""},
    {"color", "text", "cmyk 0 0 0 100", true, false, ""},
    {"paint", "enum", "fill", true, false, "fill|stroke"},
    {"x", "length", "0mm", true, false, ""},
    {"y", "length", "0mm", true, false, ""},
    {"anchor", "enum", "center", true, false, "lower_left|lower_right|upper_left|upper_right|center|left|right|top|bottom"},
    {"relative_to", "enum", "crop", true, false, "media|crop|bleed|trim|art"},
    {"rotation", "angle", "0", true, false, ""},
    {"layer", "text", "", false, false, ""},
    {"position", "enum", "front", true, false, "front|back"},
    {"pages", "text", "all", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_place_text[] = {
    {"text", "text", "", false, true, ""},
    {"font", "text", "Helvetica", true, false, ""},
    {"size", "length", "10pt", true, false, ""},
    {"color", "text", "cmyk 0 0 0 100", true, false, ""},
    {"x", "length", "0mm", true, false, ""},
    {"y", "length", "0mm", true, false, ""},
    {"anchor", "enum", "center", true, false, "lower_left|lower_right|upper_left|upper_right|center|left|right|top|bottom"},
    {"relative_to", "enum", "crop", true, false, "media|crop|bleed|trim|art"},
    {"rotation", "angle", "0", true, false, ""},
    {"layer", "text", "", false, false, ""},
    {"position", "enum", "front", true, false, "front|back"},
    {"pages", "text", "all", true, false, ""},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_spot_normalise_names[] = {
    {"system", "enum", "any", true, false, "any|pantone|hks"},
};

static const ParamDef kParams_f_spot_registration_to_black[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_spot_rename[] = {
    {"from", "text", "", false, true, ""},
    {"to", "text", "", false, true, ""},
};

static const ParamDef kParams_f_spot_unify_definitions[] = {
    {"prefer", "enum", "most_used", true, false, "most_used|first"},
};

static const ParamDef kParams_f_text_add_invisible_copy[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_text_set_render_mode[] = {
    {"mode", "enum", "fill", true, false, "fill|stroke|fill_and_stroke|invisible"},
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_transparency_flatten[] = {
    {"raster_resolution", "resolution", "300ppi", true, false, ""},
};

static const ParamDef kParams_f_transparency_remove_unneeded_groups[] = {
    {"only_where", "list", "", false, false, ""},
};

static const ParamDef kParams_f_transparency_set_blend_space[] = {
    {"space", "enum", "output_intent", true, false, "output_intent|CMYK|sRGB|Gray"},
    {"only_where", "list", "", false, false, ""},
};

static const CheckDef kChecks[] = {
    {"action.broken_destination", "action", "Destination points nowhere", "bool", Domain::kDoc, "is", kParams_c_action_broken_destination, 1},
    {"action.destination", "action", "Destination fit mode", "enum", Domain::kDoc, "==|!=|in|not_in", kParams_c_action_destination, 1},
    {"action.present", "action", "Action present", "bool", Domain::kDoc, "is", kParams_c_action_present, 3},
    {"action.trigger", "action", "Additional-action trigger", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_action_trigger, 1},
    {"action.type", "action", "Action type", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"action.uri", "action", "Link target URI", "text", Domain::kAnnot, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", nullptr, 0},
    {"annotation.appearance", "annotation", "Appearance stream problems", "list", Domain::kAnnot, "includes|excludes|count", nullptr, 0},
    {"annotation.entry", "annotation", "Annotation entry", "text", Domain::kAnnot, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_annotation_entry, 1},
    {"annotation.flags", "annotation", "Annotation flags", "list", Domain::kAnnot, "includes|excludes|count", nullptr, 0},
    {"annotation.interactive", "annotation", "Interactive annotation", "bool", Domain::kAnnot, "is", nullptr, 0},
    {"annotation.link_colour_only", "annotation", "Link marked by colour only", "bool", Domain::kAnnot, "is", nullptr, 0},
    {"annotation.link_text", "annotation", "Link text", "text", Domain::kAnnot, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", nullptr, 0},
    {"annotation.minimum_pdf_version", "annotation", "PDF version an annotation type needs", "version", Domain::kAnnot, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"annotation.multimedia", "annotation", "3D and media annotation problems", "list", Domain::kAnnot, "includes|excludes|count", nullptr, 0},
    {"annotation.outside_crop", "annotation", "Annotation outside the visible page", "enum", Domain::kAnnot, "==|!=|in|not_in", nullptr, 0},
    {"annotation.type", "annotation", "Annotation type", "enum", Domain::kAnnot, "==|!=|in|not_in", nullptr, 0},
    {"attachment.count", "attachment", "Number of embedded files", "count", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_attachment_count, 2},
    {"attachment.is_pdf", "attachment", "Embedded file is a PDF", "bool", Domain::kDoc, "is", kParams_c_attachment_is_pdf, 1},
    {"attachment.problem", "attachment", "Embedded file problems", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"attachment.property", "attachment", "Embedded file property", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_attachment_property, 1},
    {"barcode.dimension", "barcode", "Barcode dimension", "length", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_barcode_dimension, 2},
    {"barcode.found", "barcode", "Barcode found", "bool", Domain::kDoc, "is", kParams_c_barcode_found, 1},
    {"barcode.property", "barcode", "Barcode property", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_barcode_property, 1},
    {"barcode.quiet_zone_sufficient", "barcode", "Quiet zone is large enough", "bool", Domain::kDoc, "is", nullptr, 0},
    {"color.black_point_compensation", "color", "Black point compensation setting", "enum", Domain::kAny, "==|!=|in|not_in", nullptr, 0},
    {"color.channel_spread", "color", "Difference between inks", "percent", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_color_channel_spread, 1},
    {"color.component_count", "color", "Number of colour components", "count", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_color_component_count, 2},
    {"color.device_space_rule", "color", "Device colour without a defined meaning", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"color.devicen", "color", "DeviceN and NChannel details", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"color.devicen_colorants", "color", "DeviceN colorant names", "list", Domain::kAny, "includes|excludes|count", kParams_c_color_devicen_colorants, 1},
    {"color.ink", "color", "Amount of one ink", "percent", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_color_ink, 2},
    {"color.ink_mix", "color", "Ink combination", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_color_ink_mix, 1},
    {"color.inks_used", "color", "Number of inks used", "count", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_color_inks_used, 2},
    {"color.overprint", "color", "Overprint", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_color_overprint, 1},
    {"color.rendering_intent", "color", "Rendering intent", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_color_rendering_intent, 1},
    {"color.space", "color", "Colour space", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_color_space, 3},
    {"color.spot", "color", "Spot colour", "text", Domain::kAny, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_color_spot, 2},
    {"color.total_ink", "color", "Total ink of the object", "percent", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_color_total_ink, 2},
    {"color.unused_resource", "color", "Unused colour resources", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"color.value", "color", "Colour value", "text", Domain::kAny, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_color_value, 2},
    {"conformance.claim", "conformance", "Declared conformance", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_conformance_claim, 2},
    {"conformance.meets", "conformance", "Conforms to a standard", "bool", Domain::kDoc, "is", kParams_c_conformance_meets, 2},
    {"content.box_relation", "content", "Object position against a page box", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_content_box_relation, 3},
    {"content.clipped", "content", "Clipping", "enum", Domain::kAny, "==|!=|in|not_in", nullptr, 0},
    {"content.context", "content", "Where the object sits", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"content.contrast_sufficient", "content", "Enough contrast against the background", "bool", Domain::kAny, "is", kParams_c_content_contrast_sufficient, 7},
    {"content.dash_length", "content", "Dash and gap lengths", "length", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_dash_length, 3},
    {"content.distance_to_box", "content", "Distance to a box edge", "length", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_distance_to_box, 3},
    {"content.distorted", "content", "Distorted placement", "bool", Domain::kAny, "is", nullptr, 0},
    {"content.in_area", "content", "Object in an area", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_content_in_area, 6},
    {"content.kind", "content", "Object kind", "enum", Domain::kAny, "==|!=|in|not_in", nullptr, 0},
    {"content.outside_share", "content", "Share of the object outside a box", "percent", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_outside_share, 1},
    {"content.painting", "content", "How the object is painted", "enum", Domain::kAny, "==|!=|in|not_in", nullptr, 0},
    {"content.path_measure", "content", "Path geometry", "count", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_path_measure, 1},
    {"content.relation", "content", "Relation to other objects", "list", Domain::kAny, "includes|excludes|count", kParams_c_content_relation, 3},
    {"content.shading", "content", "Shading properties", "text", Domain::kAny, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_content_shading, 1},
    {"content.shape_relation", "content", "Relation to a shape", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_content_shape_relation, 3},
    {"content.sharp_corners", "content", "Sharp corners and tight curves", "count", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_sharp_corners, 3},
    {"content.size", "content", "Object size", "length", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_size, 2},
    {"content.stroke_geometry", "content", "Stroke geometry", "length", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_stroke_geometry, 2},
    {"content.stroke_style", "content", "Line style", "text", Domain::kAny, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_content_stroke_style, 1},
    {"content.stroke_width", "content", "Line width", "length", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_content_stroke_width, 2},
    {"content.visibility", "content", "Visible result", "enum", Domain::kAny, "==|!=|in|not_in", nullptr, 0},
    {"document.binding", "document", "Reading direction / binding side", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"document.colour_spaces", "document", "Colour spaces used", "list", Domain::kDoc, "includes|excludes|count", kParams_c_document_colour_spaces, 1},
    {"document.content_stream_size", "document", "Largest page content size", "bytes", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_document_content_stream_size, 1},
    {"document.created_by_print_driver", "document", "Created by a print-to-PDF driver", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.damaged", "document", "File is damaged", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.data_after_eof", "document", "Data after end of file", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.extension_level", "document", "Developer extension level", "number", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_document_extension_level, 1},
    {"document.extension_version_mismatch", "document", "Extension base version mismatch", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.file_size", "document", "File size", "bytes", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_document_file_size, 1},
    {"document.form_nesting", "document", "Form XObject nesting depth", "count", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"document.has", "document", "Document contains a feature", "bool", Domain::kDoc, "is", kParams_c_document_has, 1},
    {"document.header_conforms", "document", "File header conforms to a standard", "bool", Domain::kDoc, "is", kParams_c_document_header_conforms, 1},
    {"document.id", "document", "File identifier", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"document.job_options", "document", "Embedded creation settings", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_document_job_options, 1},
    {"document.language", "document", "Document language", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_document_language, 1},
    {"document.largest_stream", "document", "Largest stream", "bytes", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_document_largest_stream, 1},
    {"document.linearized", "document", "Linearized for fast web view", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.mark_info_extra_keys", "document", "MarkInfo has other entries", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.marked", "document", "Marked as tagged", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.needs_rendering", "document", "NeedsRendering is true", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.open_view", "document", "Initial view settings", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_document_open_view, 1},
    {"document.page_count", "document", "Number of pages", "count", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"document.pages_uniform", "document", "All pages alike", "bool", Domain::kDoc, "is", kParams_c_document_pages_uniform, 3},
    {"document.pdf_version", "document", "PDF version", "version", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_document_pdf_version, 1},
    {"document.portfolio", "document", "Portfolio (collection)", "bool", Domain::kDoc, "is", nullptr, 0},
    {"document.requirements", "document", "Declares viewer requirements", "bool", Domain::kDoc, "is", kParams_c_document_requirements, 1},
    {"document.script", "document", "Custom script test", "bool", Domain::kDoc, "is", kParams_c_document_script, 1},
    {"document.stream_compression", "document", "Stream compression", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"document.stream_layout", "document", "Stream dictionary problems", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"document.title", "document", "Document title", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"document.trailer", "document", "Trailer entries", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"document.uses_object_streams", "document", "Uses compressed object streams", "bool", Domain::kDoc, "is", kParams_c_document_uses_object_streams, 1},
    {"document.version_conflict", "document", "Header and catalog versions differ", "bool", Domain::kDoc, "is", nullptr, 0},
    {"einvoice.metadata", "einvoice", "Invoice metadata in XMP", "bool", Domain::kDoc, "is", nullptr, 0},
    {"einvoice.present", "einvoice", "Electronic invoice attached", "count", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"einvoice.relationship_valid", "einvoice", "Invoice relationship is valid", "bool", Domain::kDoc, "is", nullptr, 0},
    {"einvoice.valid", "einvoice", "Invoice XML is valid", "bool", Domain::kDoc, "is", nullptr, 0},
    {"font.all_text_unicode", "font", "All text of the font maps to Unicode", "bool", Domain::kFont, "is", nullptr, 0},
    {"font.embedding", "font", "Font embedding", "enum", Domain::kFont, "==|!=|in|not_in", nullptr, 0},
    {"font.encoding", "font", "Font encoding", "enum", Domain::kFont, "==|!=|in|not_in", nullptr, 0},
    {"font.flags", "font", "Font descriptor flags", "list", Domain::kFont, "includes|excludes|count", nullptr, 0},
    {"font.has_to_unicode", "font", "Font has ToUnicode map", "bool", Domain::kFont, "is", nullptr, 0},
    {"font.license", "font", "Embedding licence", "enum", Domain::kFont, "==|!=|in|not_in", nullptr, 0},
    {"font.metric", "font", "Font metric", "number", Domain::kFont, "<|<=|==|!=|>=|>|multiple_of", kParams_c_font_metric, 1},
    {"font.name", "font", "Font name", "text", Domain::kFont, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_font_name, 1},
    {"font.name_problem", "font", "Font name problems", "list", Domain::kFont, "includes|excludes|count", nullptr, 0},
    {"font.problem", "font", "Font program and dictionary problems", "list", Domain::kFont, "includes|excludes|count", kParams_c_font_problem, 1},
    {"font.type", "font", "Font type", "enum", Domain::kFont, "==|!=|in|not_in", nullptr, 0},
    {"form.field_flags", "form", "Form field flags", "list", Domain::kAnnot, "includes|excludes|count", nullptr, 0},
    {"form.field_name", "form", "Form field name", "text", Domain::kAnnot, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_form_field_name, 1},
    {"form.field_type", "form", "Form field type", "enum", Domain::kAnnot, "==|!=|in|not_in", nullptr, 0},
    {"form.field_value", "form", "Form field value", "text", Domain::kAnnot, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", nullptr, 0},
    {"form.xfa", "form", "XFA form", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"icc.header", "icc", "ICC profile header field", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_icc_header, 2},
    {"icc.matches_output_intent", "icc", "Profile agrees with an output intent", "bool", Domain::kDoc, "is", kParams_c_icc_matches_output_intent, 2},
    {"icc.max_ink", "icc", "Ink limit of a profile", "percent", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_icc_max_ink, 1},
    {"icc.tag", "icc", "ICC profile tag", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_icc_tag, 2},
    {"image.abbreviated_filter_names", "image", "Abbreviated names in inline images", "bool", Domain::kImage, "is", nullptr, 0},
    {"image.alternates", "image", "Alternate images", "list", Domain::kImage, "includes|excludes|count", nullptr, 0},
    {"image.jbig2", "image", "JBIG2 properties", "list", Domain::kImage, "includes|excludes|count", nullptr, 0},
    {"image.jpeg2000", "image", "JPEG 2000 properties", "text", Domain::kImage, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_image_jpeg2000, 1},
    {"image.opi", "image", "OPI comment value", "text", Domain::kImage, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_image_opi, 2},
    {"image.property", "image", "Image property", "text", Domain::kImage, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_image_property, 2},
    {"image.resolution", "image", "Effective image resolution", "resolution", Domain::kImage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_image_resolution, 2},
    {"image.rotation", "image", "Image rotation", "angle", Domain::kImage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_image_rotation, 1},
    {"layer.configuration", "layer", "Layer configuration problems", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"layer.configuration_name", "layer", "Configuration name and order", "list", Domain::kDoc, "includes|excludes|count", kParams_c_layer_configuration_name, 1},
    {"layer.membership", "layer", "Layer membership", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"layer.processing_step", "layer", "Processing step layers", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"layer.processing_step_problem", "layer", "Processing step problems", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"layer.property", "layer", "Layer property", "text", Domain::kAny, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_layer_property, 2},
    {"layer.visibility_policy", "layer", "Membership visibility policy", "enum", Domain::kAny, "==|!=|in|not_in", nullptr, 0},
    {"layer.visible", "layer", "Layer visibility", "bool", Domain::kAny, "is", kParams_c_layer_visible, 1},
    {"metadata.info", "metadata", "Document information entry", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_metadata_info, 1},
    {"metadata.info_keys", "metadata", "Information dictionary contents", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"metadata.info_matches_xmp", "metadata", "Information entry agrees with XMP", "bool", Domain::kDoc, "is", kParams_c_metadata_info_matches_xmp, 1},
    {"metadata.info_problem", "metadata", "Information dictionary value problems", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"metadata.object_has_xmp", "metadata", "Object carries its own XMP", "bool", Domain::kDoc, "is", kParams_c_metadata_object_has_xmp, 1},
    {"metadata.xmp_document_id_is_uuid", "metadata", "Document ID is a UUID", "bool", Domain::kDoc, "is", nullptr, 0},
    {"metadata.xmp_extension_schema", "metadata", "XMP extension schema", "enum", Domain::kDoc, "==|!=|in|not_in", kParams_c_metadata_xmp_extension_schema, 1},
    {"metadata.xmp_history_incomplete", "metadata", "Incomplete history entry", "bool", Domain::kDoc, "is", kParams_c_metadata_xmp_history_incomplete, 2},
    {"metadata.xmp_identification", "metadata", "Standard identification in XMP", "list", Domain::kDoc, "includes|excludes|count", kParams_c_metadata_xmp_identification, 1},
    {"metadata.xmp_namespaces", "metadata", "XMP namespaces", "list", Domain::kDoc, "includes|excludes|count", kParams_c_metadata_xmp_namespaces, 1},
    {"metadata.xmp_prefix", "metadata", "Prefix used for a namespace", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_metadata_xmp_prefix, 2},
    {"metadata.xmp_problem", "metadata", "XMP packet problems", "list", Domain::kDoc, "includes|excludes|count", kParams_c_metadata_xmp_problem, 1},
    {"metadata.xmp_property", "metadata", "XMP property value", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_metadata_xmp_property, 2},
    {"metadata.xmp_property_known", "metadata", "XMP property is defined", "bool", Domain::kDoc, "is", kParams_c_metadata_xmp_property_known, 2},
    {"metadata.xmp_schema_file", "metadata", "XMP schema description file", "enum", Domain::kDoc, "==|!=|in|not_in", kParams_c_metadata_xmp_schema_file, 1},
    {"metadata.xmp_size", "metadata", "XMP packet size", "bytes", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_metadata_xmp_size, 2},
    {"metadata.xmp_value_type", "metadata", "XMP value matches its definition", "bool", Domain::kDoc, "is", kParams_c_metadata_xmp_value_type, 2},
    {"output_intent.colour_exchange", "output_intent", "Embedded colour exchange data", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_output_intent_colour_exchange, 1},
    {"output_intent.count", "output_intent", "Number of output intents", "count", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_output_intent_count, 1},
    {"output_intent.entry", "output_intent", "Output intent entry", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_output_intent_entry, 2},
    {"output_intent.profile_reference", "output_intent", "Referenced output profile", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"page.aspect_ratio", "page", "Aspect ratio", "ratio", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_page_aspect_ratio, 1},
    {"page.box_offset", "page", "Page box offset", "length", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_page_box_offset, 4},
    {"page.box_present", "page", "Page box present", "bool", Domain::kPage, "is", kParams_c_page_box_present, 1},
    {"page.box_relation", "page", "Relation between two page boxes", "enum", Domain::kPage, "==|!=|in|not_in", kParams_c_page_box_relation, 2},
    {"page.box_size", "page", "Page box dimension", "length", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_page_box_size, 4},
    {"page.boxes_nested", "page", "Page boxes are properly nested", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.content_stream", "page", "Page content stream", "text", Domain::kPage, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_page_content_stream, 1},
    {"page.empty", "page", "Page is empty", "bool", Domain::kPage, "is", kParams_c_page_empty, 1},
    {"page.has_measure_viewport", "page", "Geospatial or measure viewport", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.has_no_text", "page", "Page has no text", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.in_range", "page", "Page is in a range", "bool", Domain::kPage, "is", kParams_c_page_in_range, 1},
    {"page.label", "page", "Page label", "text", Domain::kPage, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_page_label, 1},
    {"page.media_origin_not_zero", "page", "Media box not at origin", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.mostly_rotated_text", "page", "Most text is rotated", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.number", "page", "Page number", "count", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"page.orientation", "page", "Orientation", "enum", Domain::kPage, "==|!=|in|not_in", kParams_c_page_orientation, 1},
    {"page.output_intent", "page", "Page-level output intent", "enum", Domain::kPage, "==|!=|in|not_in", kParams_c_page_output_intent, 1},
    {"page.parity", "page", "Odd or even page", "enum", Domain::kPage, "==|!=|in|not_in", nullptr, 0},
    {"page.private_data", "page", "Private application data", "list", Domain::kPage, "includes|excludes|count", nullptr, 0},
    {"page.processing_steps", "page", "Processing steps on the page", "list", Domain::kPage, "includes|excludes|count", nullptr, 0},
    {"page.processing_steps_entry", "page", "Page declares processing steps", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.resources_missing", "page", "Resources missing", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.rotation", "page", "Page rotation", "angle", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"page.single_image", "page", "Page is one image", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.size", "page", "Page size", "size", Domain::kPage, "==|!=|fits_within|exceeds", kParams_c_page_size, 5},
    {"page.text_contains", "page", "Page text contains", "text", Domain::kPage, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", nullptr, 0},
    {"page.transparency_group", "page", "Page transparency group", "list", Domain::kPage, "includes|excludes|count", nullptr, 0},
    {"page.trim_or_art_only", "page", "Exactly one of trim and art box", "bool", Domain::kPage, "is", nullptr, 0},
    {"page.unmappable_text", "page", "Share of text without Unicode", "percent", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"page.user_unit", "page", "Page scaling (user unit)", "number", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"page.uses_transparency", "page", "Page uses transparency", "bool", Domain::kPage, "is", nullptr, 0},
    {"print.bleed", "print", "Bleed amount", "length", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_print_bleed, 2},
    {"print.bleed_missing", "print", "Content does not bleed", "bool", Domain::kPage, "is", kParams_c_print_bleed_missing, 3},
    {"print.empty_area", "print", "Empty area on the page", "bool", Domain::kPage, "is", kParams_c_print_empty_area, 3},
    {"print.flatness", "print", "Flatness tolerance", "number", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"print.halftone", "print", "Halftone screen", "text", Domain::kAny, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_print_halftone, 2},
    {"print.images_on_cmy_only", "print", "Images on colour plates only", "bool", Domain::kDoc, "is", kParams_c_print_images_on_cmy_only, 1},
    {"print.ink_area", "print", "Area above an ink amount", "length", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_print_ink_area, 4},
    {"print.ink_coverage", "print", "Total ink coverage", "percent", Domain::kPage, "<|<=|==|!=|>=|>|multiple_of", kParams_c_print_ink_coverage, 4},
    {"print.mixing_hints", "print", "DeviceN mixing hints consistency", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"print.plates", "print", "Separations", "list", Domain::kDoc, "includes|excludes|count", kParams_c_print_plates, 3},
    {"print.registration_used", "print", "Registration colour prints", "bool", Domain::kPage, "is", nullptr, 0},
    {"print.rendering_functions", "print", "Device-dependent rendering functions", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"print.spot_naming", "print", "Spot colour naming problems", "list", Domain::kDoc, "includes|excludes|count", kParams_c_print_spot_naming, 1},
    {"print.trap_network", "print", "Trap network annotations", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"print.trapped", "print", "Trapped flag", "enum", Domain::kDoc, "==|!=|in|not_in", kParams_c_print_trapped, 1},
    {"print.used_area", "print", "Area that carries ink", "size", Domain::kPage, "==|!=|fits_within|exceeds", kParams_c_print_used_area, 1},
    {"print.uses_plates", "print", "Page prints on given plates", "bool", Domain::kPage, "is", kParams_c_print_uses_plates, 3},
    {"scope.every", "scope", "Every item", "bool", Domain::kDoc, "is", kParams_c_scope_every, 1},
    {"seal.record", "seal", "Kura check seal record", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_seal_record, 1},
    {"seal.state", "seal", "Kura check seal", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"security.encryption", "security", "Encryption", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"security.encryption_dictionary", "security", "Encryption dictionary details", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_security_encryption_dictionary, 1},
    {"security.permission", "security", "Permission denied", "bool", Domain::kDoc, "is", kParams_c_security_permission, 1},
    {"signature.dictionary", "signature", "Signature dictionary entry", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_signature_dictionary, 1},
    {"signature.flags", "signature", "Signature flags", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"signature.permissions", "signature", "Document permissions dictionary", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"signature.present", "signature", "Signature fields", "bool", Domain::kDoc, "is", kParams_c_signature_present, 1},
    {"structure.alternate_text", "structure", "Alternative descriptions", "list", Domain::kDoc, "includes|excludes|count", kParams_c_structure_alternate_text, 1},
    {"structure.artifact", "structure", "Artifact type", "text", Domain::kAny, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_structure_artifact, 1},
    {"structure.content_tagging", "structure", "Content is tagged or artifact", "enum", Domain::kAny, "==|!=|in|not_in", nullptr, 0},
    {"structure.element", "structure", "Structure element", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_structure_element, 1},
    {"structure.empty_block", "structure", "Empty block element", "bool", Domain::kDoc, "is", nullptr, 0},
    {"structure.heading_level", "structure", "Heading level", "count", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_structure_heading_level, 1},
    {"structure.headings", "structure", "Heading structure", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"structure.language_change", "structure", "Unmarked change of language", "list", Domain::kDoc, "includes|excludes|count", kParams_c_structure_language_change, 1},
    {"structure.language_unknown", "structure", "Natural language cannot be determined", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"structure.list", "structure", "List structure", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_structure_list, 1},
    {"structure.nesting", "structure", "Annotation and field nesting", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"structure.notes", "structure", "Note identifiers", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"structure.standard_type", "structure", "Uses standard structure types", "list", Domain::kDoc, "includes|excludes|count", kParams_c_structure_standard_type, 1},
    {"structure.table", "structure", "Table structure", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_structure_table, 1},
    {"structure.toc", "structure", "Table of contents structure", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"structure.tree", "structure", "Structure tree problems", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"syntax.broken_reference", "syntax", "Reference to a missing object", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"syntax.content_stream_error", "syntax", "Content stream violates the specification", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"syntax.dictionary_error", "syntax", "Dictionary or array violates the specification", "list", Domain::kDoc, "includes|excludes|count", kParams_c_syntax_dictionary_error, 1},
    {"syntax.limit", "syntax", "Implementation limit", "number", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", kParams_c_syntax_limit, 1},
    {"syntax.linearization_id_mismatch", "syntax", "Linearized file has two different IDs", "bool", Domain::kDoc, "is", nullptr, 0},
    {"syntax.object_count", "syntax", "Number of indirect objects", "count", Domain::kDoc, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"syntax.object_layout", "syntax", "Indirect object layout", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"syntax.postscript", "syntax", "Embedded PostScript", "enum", Domain::kDoc, "==|!=|in|not_in", nullptr, 0},
    {"syntax.resource_error", "syntax", "Damaged embedded resource", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"syntax.string_encoding", "syntax", "String and name encoding", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"syntax.unknown_operator", "syntax", "Operator unknown to a PDF version", "bool", Domain::kDoc, "is", kParams_c_syntax_unknown_operator, 1},
    {"syntax.xref_layout", "syntax", "Cross-reference section layout", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"text.geometry", "text", "Text orientation", "angle", Domain::kText, "<|<=|==|!=|>=|>|multiple_of", kParams_c_text_geometry, 1},
    {"text.glyph", "text", "Glyph state", "enum", Domain::kText, "==|!=|in|not_in", nullptr, 0},
    {"text.legacy_codes", "text", "Legacy character codes", "bool", Domain::kText, "is", nullptr, 0},
    {"text.negative_size", "text", "Negative font size", "bool", Domain::kText, "is", nullptr, 0},
    {"text.rendering", "text", "How text is rendered", "enum", Domain::kText, "==|!=|in|not_in", nullptr, 0},
    {"text.size", "text", "Text size", "length", Domain::kText, "<|<=|==|!=|>=|>|multiple_of", kParams_c_text_size, 2},
    {"text.state", "text", "Text state value", "number", Domain::kText, "<|<=|==|!=|>=|>|multiple_of", kParams_c_text_state, 1},
    {"text.type3_glyph_operator", "text", "Type 3 glyph metrics operator", "enum", Domain::kText, "==|!=|in|not_in", nullptr, 0},
    {"text.unicode", "text", "Unicode mapping", "enum", Domain::kText, "==|!=|in|not_in", nullptr, 0},
    {"text.unicode_conflict", "text", "Unicode mapping disagrees", "list", Domain::kText, "includes|excludes|count", nullptr, 0},
    {"transparency.alpha", "transparency", "Constant opacity", "number", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", kParams_c_transparency_alpha, 1},
    {"transparency.blend_mode", "transparency", "Blend mode", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_transparency_blend_mode, 1},
    {"transparency.group", "transparency", "Transparency group", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"transparency.group_colour_space", "transparency", "Group blending colour space", "enum", Domain::kAny, "==|!=|in|not_in", kParams_c_transparency_group_colour_space, 1},
    {"transparency.group_depth", "transparency", "Group nesting depth", "count", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"transparency.keys_present", "transparency", "Transparency keys present", "bool", Domain::kAny, "is", nullptr, 0},
    {"transparency.soft_mask", "transparency", "Soft mask properties", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"transparency.used", "transparency", "Object is transparent", "bool", Domain::kAny, "is", kParams_c_transparency_used, 2},
    {"variable_data.document_parts", "variable_data", "Document part structure", "list", Domain::kDoc, "includes|excludes|count", nullptr, 0},
    {"variable_data.encapsulated", "variable_data", "Encapsulated graphics", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"variable_data.part_value", "variable_data", "Document part values", "text", Domain::kDoc, "==|!=|in|not_in|contains|starts_with|ends_with|matches|finds|occurs_in", kParams_c_variable_data_part_value, 2},
    {"variable_data.xobject_uses", "variable_data", "XObject reuse count", "count", Domain::kAny, "<|<=|==|!=|>=|>|multiple_of", nullptr, 0},
    {"xobject.form_problem", "xobject", "Form XObject problems", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
    {"xobject.reference", "xobject", "Reference XObject", "list", Domain::kAny, "includes|excludes|count", nullptr, 0},
};

static const FixDef kFixes[] = {
    {"annotation.handle", "annotation", "Handle annotations", kParams_f_annotation_handle, 2},
    {"bleed.around_shape", "bleed", "Bleed around a die line", kParams_f_bleed_around_shape, 4},
    {"bleed.generate", "bleed", "Generate bleed", kParams_f_bleed_generate, 3},
    {"color.convert", "color", "Convert colours", kParams_f_color_convert, 12},
    {"color.embed_measurements", "color", "Embed spot colour measurements", kParams_f_color_embed_measurements, 1},
    {"color.icc_to_device", "color", "ICC to device colour", kParams_f_color_icc_to_device, 1},
    {"color.icc_to_v2", "color", "Downgrade ICC v4 profiles", nullptr, 0},
    {"color.map", "color", "Replace colours", kParams_f_color_map, 5},
    {"color.nchannel_to_devicen", "color", "NChannel to DeviceN", nullptr, 0},
    {"color.process_to_spot", "color", "Process colour to spot", kParams_f_color_process_to_spot, 3},
    {"color.split_devicen", "color", "Split DeviceN", kParams_f_color_split_devicen, 1},
    {"color.to_library_spot", "color", "Match colours to a spot library", kParams_f_color_to_library_spot, 2},
    {"content.bring_to_front", "content", "Bring objects to front", kParams_f_content_bring_to_front, 2},
    {"content.fill_as_stroke", "content", "Thin fills to strokes", kParams_f_content_fill_as_stroke, 2},
    {"content.fix_negative_dash", "content", "Repair negative dash values", kParams_f_content_fix_negative_dash, 1},
    {"content.remove_outside", "content", "Remove content outside a box", kParams_f_content_remove_outside, 2},
    {"content.smooth_lines_to_curves", "content", "Smooth polylines into curves", kParams_f_content_smooth_lines_to_curves, 1},
    {"document.associate_files", "document", "Associate embedded files", kParams_f_document_associate_files, 1},
    {"document.linearize", "document", "Optimise for web", nullptr, 0},
    {"document.remove_conformance_claim", "document", "Remove a conformance claim", kParams_f_document_remove_conformance_claim, 1},
    {"document.set_info", "document", "Set document information", kParams_f_document_set_info, 4},
    {"document.set_view", "document", "Set opening view", kParams_f_document_set_view, 7},
    {"font.rename", "font", "Rename fonts", kParams_f_font_rename, 2},
    {"font.to_outlines", "font", "Convert text to outlines", kParams_f_font_to_outlines, 1},
    {"form.generate_appearances", "form", "Generate field appearances", nullptr, 0},
    {"graphics.remove", "graphics", "Remove graphics state settings", kParams_f_graphics_remove, 2},
    {"graphics.set", "graphics", "Set graphics state values", kParams_f_graphics_set, 9},
    {"image.downsample", "image", "Downsample images", kParams_f_image_downsample, 3},
    {"image.expand_filter_names", "image", "Expand abbreviated filter names", nullptr, 0},
    {"image.merge_fragments", "image", "Merge image fragments", kParams_f_image_merge_fragments, 1},
    {"image.recompress", "image", "Recompress images", kParams_f_image_recompress, 1},
    {"image.set_interpolation", "image", "Image interpolation", kParams_f_image_set_interpolation, 1},
    {"image.sharpen", "image", "Sharpen images", kParams_f_image_sharpen, 2},
    {"image.upsample", "image", "Upsample low-resolution images", kParams_f_image_upsample, 2},
    {"ink.dot_gain", "ink", "Compensate dot gain", kParams_f_ink_dot_gain, 1},
    {"ink.knockout_white", "ink", "Knock out white", kParams_f_ink_knockout_white, 2},
    {"ink.min_line_width", "ink", "Thicken hairlines", kParams_f_ink_min_line_width, 2},
    {"ink.overprint_black", "ink", "Overprint solid black", kParams_f_ink_overprint_black, 2},
    {"ink.white_plate", "ink", "Create a white plate", kParams_f_ink_white_plate, 4},
    {"layer.add_view", "layer", "Add a layer view", kParams_f_layer_add_view, 4},
    {"layer.flatten", "layer", "Flatten layers", kParams_f_layer_flatten, 1},
    {"layer.move_objects", "layer", "Move objects to a layer", kParams_f_layer_move_objects, 3},
    {"layer.set", "layer", "Change layer properties", kParams_f_layer_set, 11},
    {"link.from_text", "structure", "Links from URLs in text", nullptr, 0},
    {"metadata.uncompress_xmp", "metadata", "Store XMP uncompressed", nullptr, 0},
    {"outline.from_headings", "structure", "Bookmarks from headings", kParams_f_outline_from_headings, 1},
    {"outline.repair", "structure", "Repair bookmarks", nullptr, 0},
    {"output_intent.remove_profile", "output_intent", "Remove output profile", nullptr, 0},
    {"output_intent.set", "output_intent", "Set output intent", kParams_f_output_intent_set, 6},
    {"page.add_marks", "page", "Add printer's marks", kParams_f_page_add_marks, 5},
    {"page.crop_to", "page", "Crop to content", kParams_f_page_crop_to, 2},
    {"page.mirror", "page", "Mirror pages", kParams_f_page_mirror, 2},
    {"page.move_origin", "page", "Move media box to origin", kParams_f_page_move_origin, 1},
    {"page.rasterize", "page", "Rasterize page", kParams_f_page_rasterize, 2},
    {"page.remove_user_unit", "page", "Remove page scaling", kParams_f_page_remove_user_unit, 1},
    {"page.repair_resources", "page", "Repair missing resources", kParams_f_page_repair_resources, 1},
    {"page.rotate", "page", "Rotate pages", kParams_f_page_rotate, 3},
    {"page.scale", "page", "Scale pages", kParams_f_page_scale, 5},
    {"page.set_box", "page", "Set a page box", kParams_f_page_set_box, 8},
    {"place.barcode", "place", "Place barcode", kParams_f_place_barcode, 14},
    {"place.circle", "place", "Place circle", kParams_f_place_circle, 12},
    {"place.html", "place", "Place HTML", kParams_f_place_html, 11},
    {"place.pdf", "place", "Place PDF page", kParams_f_place_pdf, 12},
    {"place.rectangle", "place", "Place rectangle", kParams_f_place_rectangle, 13},
    {"place.text", "place", "Place text", kParams_f_place_text, 13},
    {"spot.normalise_names", "spot", "Normalise spot names", kParams_f_spot_normalise_names, 1},
    {"spot.registration_to_black", "spot", "Registration to black", kParams_f_spot_registration_to_black, 1},
    {"spot.rename", "spot", "Rename spot colours", kParams_f_spot_rename, 2},
    {"spot.unify_definitions", "spot", "Unify spot definitions", kParams_f_spot_unify_definitions, 1},
    {"structure.actual_text_for_symbols", "structure", "Actual text for symbol fonts", nullptr, 0},
    {"structure.mark_header_footer", "structure", "Mark headers and footers as artifacts", nullptr, 0},
    {"structure.resolve_role_map", "structure", "Resolve role map", nullptr, 0},
    {"text.add_invisible_copy", "text", "Add searchable text layer", kParams_f_text_add_invisible_copy, 1},
    {"text.set_render_mode", "text", "Set text render mode", kParams_f_text_set_render_mode, 2},
    {"ink.flatten_overprint", "ink", "Flatten overprint", nullptr, 0},
    {"transparency.flatten", "transparency", "Flatten transparency", kParams_f_transparency_flatten, 1},
    {"transparency.remove_unneeded_groups", "transparency", "Remove unneeded transparency groups", kParams_f_transparency_remove_unneeded_groups, 1},
    {"transparency.set_blend_space", "transparency", "Set page blending space", kParams_f_transparency_set_blend_space, 2},
};

static const OutputCodeDef kOutputCodes[] = {
    {"CHECK_HIT", "A check found something."},
    {"CHECK_NOT_EVALUATED", "A condition could not be evaluated for this file."},
    {"PROFILE_INVALID", "The profile is not valid Kura JSON; the message names the field."},
    {"PROFILE_EMPTY", "The profile has no checks to run in check mode."},
    {"PROFILE_TARGET", "The profile's convert section chose the target standard."},
    {"VARIABLE_SET", "A variable received a value."},
    {"FIX_SCHEDULED", "A fix will run during conversion."},
    {"FIX_DONE", "A fix changed the document."},
    {"FIX_NOT_NEEDED", "A fix's only_where checks found nothing."},
    {"FIX_UNKNOWN", "The fix id is not in the catalogue."},
    {"ICC_PROFILE_MISSING", "A named ICC profile was not found."},
    {"PLAN_STEP", "A plan step ran; the message gives its result and the next step."},
    {"PLAN_STEP_FAILED", "A converting plan step failed."},
    {"PLAN_NO_CONVERSION", "The plan ran no converting step; output equals input."},
};
