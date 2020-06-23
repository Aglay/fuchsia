[TOC]

# fidl.test.json


## **PROTOCOLS**

## TestProtocol {#TestProtocol}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#127)*


### StrictXUnionHenceResponseMayBeStackAllocated {#fidl.test.json/TestProtocol.StrictXUnionHenceResponseMayBeStackAllocated}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>xu</code></td>
            <td>
                <code><a class='link' href='#StrictBoundedXUnion'>StrictBoundedXUnion</a></code>
            </td>
        </tr></table>

### FlexibleXUnionHenceResponseMustBeHeapAllocated {#fidl.test.json/TestProtocol.FlexibleXUnionHenceResponseMustBeHeapAllocated}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>xu</code></td>
            <td>
                <code><a class='link' href='#OlderSimpleUnion'>OlderSimpleUnion</a></code>
            </td>
        </tr></table>



## **STRUCTS**

### Pizza {#Pizza}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#3)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr><tr>
            <td><code>toppings</code></td>
            <td>
                <code>vector&lt;string&gt;</code>
            </td>
            <td></td>
            <td>No default</td>
        </tr>
</table>

### Pasta {#Pasta}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#7)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr><tr>
            <td><code>sauce</code></td>
            <td>
                <code>string[16]</code>
            </td>
            <td></td>
            <td>No default</td>
        </tr>
</table>

### NullableUnionStruct {#NullableUnionStruct}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#66)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr><tr>
            <td><code>the_union</code></td>
            <td>
                <code><a class='link' href='#Union'>Union</a>?</code>
            </td>
            <td></td>
            <td>No default</td>
        </tr>
</table>

### Empty {#Empty}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#114)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr>
</table>

### StructWithNullableXUnion {#StructWithNullableXUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#132)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr><tr>
            <td><code>x1</code></td>
            <td>
                <code><a class='link' href='#OlderSimpleUnion'>OlderSimpleUnion</a>?</code>
            </td>
            <td></td>
            <td>No default</td>
        </tr>
</table>







## **UNIONS**

### PizzaOrPasta {#PizzaOrPasta}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#11)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>pizza</code></td>
            <td>
                <code><a class='link' href='#Pizza'>Pizza</a></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>pasta</code></td>
            <td>
                <code><a class='link' href='#Pasta'>Pasta</a></code>
            </td>
            <td></td>
        </tr></table>

### ExplicitPizzaOrPasta {#ExplicitPizzaOrPasta}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#16)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code></code></td>
            <td>
                <code></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code></code></td>
            <td>
                <code></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>pizza</code></td>
            <td>
                <code><a class='link' href='#Pizza'>Pizza</a></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>pasta</code></td>
            <td>
                <code><a class='link' href='#Pasta'>Pasta</a></code>
            </td>
            <td></td>
        </tr></table>

### FlexiblePizzaOrPasta {#FlexiblePizzaOrPasta}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#23)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>pizza</code></td>
            <td>
                <code><a class='link' href='#Pizza'>Pizza</a></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>pasta</code></td>
            <td>
                <code><a class='link' href='#Pasta'>Pasta</a></code>
            </td>
            <td></td>
        </tr></table>

### StrictPizzaOrPasta {#StrictPizzaOrPasta}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#28)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>pizza</code></td>
            <td>
                <code><a class='link' href='#Pizza'>Pizza</a></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>pasta</code></td>
            <td>
                <code><a class='link' href='#Pasta'>Pasta</a></code>
            </td>
            <td></td>
        </tr></table>

### Union {#Union}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#33)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>Primitive</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>StringNeedsConstructor</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>VectorStringAlsoNeedsConstructor</code></td>
            <td>
                <code>vector&lt;string&gt;</code>
            </td>
            <td></td>
        </tr></table>

### FlexibleUnion {#FlexibleUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#39)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>Primitive</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>StringNeedsConstructor</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>VectorStringAlsoNeedsConstructor</code></td>
            <td>
                <code>vector&lt;string&gt;</code>
            </td>
            <td></td>
        </tr></table>

### StrictUnion {#StrictUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#45)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>Primitive</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>StringNeedsConstructor</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>VectorStringAlsoNeedsConstructor</code></td>
            <td>
                <code>vector&lt;string&gt;</code>
            </td>
            <td></td>
        </tr></table>

### FieldCollision {#FieldCollision}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#51)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>field_collision_tag</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr></table>

### ExplicitUnion {#ExplicitUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#55)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code></code></td>
            <td>
                <code></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>Primitive</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>StringNeedsConstructor</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr></table>

### ReverseOrdinalUnion {#ReverseOrdinalUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#61)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>second</code></td>
            <td>
                <code>uint32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>first</code></td>
            <td>
                <code>uint32</code>
            </td>
            <td></td>
        </tr></table>

### FlexibleFoo {#FlexibleFoo}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#70)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>s</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr></table>

### StrictFoo {#StrictFoo}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#75)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>s</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr></table>

### ExplicitFoo {#ExplicitFoo}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#80)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>s</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code></code></td>
            <td>
                <code></code>
            </td>
            <td></td>
        </tr></table>

### ExplicitStrictFoo {#ExplicitStrictFoo}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#86)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code></code></td>
            <td>
                <code></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>s</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr></table>

### OlderSimpleUnion {#OlderSimpleUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#92)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int64</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>f</code></td>
            <td>
                <code>float32</code>
            </td>
            <td></td>
        </tr></table>

### NewerSimpleUnion {#NewerSimpleUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#97)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int64</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>s</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>v</code></td>
            <td>
                <code>vector&lt;string&gt;</code>
            </td>
            <td></td>
        </tr></table>

### StrictSimpleXUnion {#StrictSimpleXUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#108)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>f</code></td>
            <td>
                <code>float32</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>s</code></td>
            <td>
                <code>string</code>
            </td>
            <td></td>
        </tr></table>

### XUnionContainingEmptyStruct {#XUnionContainingEmptyStruct}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#117)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>empty</code></td>
            <td>
                <code><a class='link' href='#Empty'>Empty</a></code>
            </td>
            <td></td>
        </tr></table>

### StrictBoundedXUnion {#StrictBoundedXUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#123)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code>v</code></td>
            <td>
                <code>vector&lt;uint8&gt;[10]</code>
            </td>
            <td></td>
        </tr></table>

### ExplicitXUnion {#ExplicitXUnion}
*Defined in [fidl.test.json/union.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/master/union.test.fidl#136)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr>
            <td><code></code></td>
            <td>
                <code></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code></code></td>
            <td>
                <code></code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>i</code></td>
            <td>
                <code>int64</code>
            </td>
            <td></td>
        </tr><tr>
            <td><code>f</code></td>
            <td>
                <code>float32</code>
            </td>
            <td></td>
        </tr></table>







