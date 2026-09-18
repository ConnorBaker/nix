{{#include store-object-info-v4-fixed.md}}

## Examples

### Minimal store object (content-addressed)

```json
{{#include schema/store-object-info-v4/pure.json}}
```

### Store object with an asserted NAR hash

```json
{{#include schema/store-object-info-v4/pure_asserted.json}}
```

### Store object with impure fields

```json
{{#include schema/store-object-info-v4/impure.json}}
```

### Minimal store object (empty)

```json
{{#include schema/store-object-info-v4/empty_pure.json}}
```

### Store object with all impure fields

```json
{{#include schema/store-object-info-v4/empty_impure.json}}
```

### NAR info (minimal)

```json
{{#include schema/nar-info-v3/pure.json}}
```

### NAR info (with binary cache fields)

```json
{{#include schema/nar-info-v3/impure.json}}
```

<!-- need to convert YAML to JSON first
## Raw Schema

[JSON Schema for Store Object Info v4](schema/store-object-info-v4.json)
-->
