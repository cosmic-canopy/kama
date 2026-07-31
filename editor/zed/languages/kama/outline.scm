; Zed's outline / breadcrumb view.
(type_declaration
  "type" @context
  (type_kind) @context
  name: (type_declaration_head name: (identifier) @name)) @item

(enum_declaration
  "enum" @context
  name: (type_declaration_head name: (identifier) @name)) @item

(function_declaration
  "fn" @context
  name: (identifier) @name) @item

(method_declaration
  "fn" @context
  name: (method_name) @name) @item

(constructor_declaration
  "ctor" @context
  name: (method_name) @name) @item

(destructor_declaration
  "~" @context
  name: (identifier) @name) @item

(field_declaration
  (variable_declarator name: (identifier) @name)) @item

(enum_member name: (identifier) @name) @item

(retroactive_impl_declaration
  "implements" @context
  contract: (type_name name: (identifier) @name)) @item
