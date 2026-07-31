(function_declaration body: (block) @function.inside) @function.around
(method_declaration body: (block) @function.inside) @function.around
(constructor_declaration body: (block) @function.inside) @function.around
(destructor_declaration body: (block) @function.inside) @function.around

(type_declaration body: (class_body) @class.inside) @class.around
(enum_declaration body: (enum_body) @class.inside) @class.around

[(line_comment) (block_comment)] @comment.inside
