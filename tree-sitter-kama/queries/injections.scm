; A tagged string carries its language in the tag: html"…" / sql"…". kama.l returns the tag as a distinct
; STRING_TAG token precisely so tooling can do this.
((tagged_string_literal
   tag: (string_tag) @injection.language)
 (#set! injection.include-children))

; `asm("…")` is embedded assembly, and its string is never interpolated.
((asm_statement code: (string_literal) @injection.content)
 (#set! injection.language "asm"))

; Block comments may usefully carry markup.
((block_comment) @injection.content
 (#set! injection.language "comment"))
