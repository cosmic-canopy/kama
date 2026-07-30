; Code-navigation tags (ctags-style), used by Helix's `hx --health`-adjacent tooling and by GitHub.
(function_declaration name: (identifier) @name) @definition.function
(method_declaration name: (method_name) @name) @definition.method
(type_declaration name: (type_declaration_head name: (identifier) @name)) @definition.class
(enum_declaration name: (type_declaration_head name: (identifier) @name)) @definition.enum
(constructor_declaration name: (identifier) @name) @definition.method
(constructor_declaration name: (method_name) @name) @definition.method
(call_expression function: (identifier) @name) @reference.call
