; Scopes and definitions, used by Helix for local-variable resolution.
(function_declaration) @local.scope
(method_declaration) @local.scope
(constructor_declaration) @local.scope
(block) @local.scope
(for_statement) @local.scope
(foreach_statement) @local.scope
(parallel_for_statement) @local.scope
(borrow_statement) @local.scope

(parameter name: (identifier) @local.definition.variable)
(variable_declarator name: (identifier) @local.definition.variable)
(constant_declarator name: (identifier) @local.definition.variable)
(foreach_statement name: (identifier) @local.definition.variable)
(parallel_for_statement name: (identifier) @local.definition.variable)
(borrow_binding alias: (identifier) @local.definition.variable)
(type_parameter name: (identifier) @local.definition.type)

(identifier) @local.reference
