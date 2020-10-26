[TOC]

# fidl.test.protocols


## **PROTOCOLS**

## WithAndWithoutRequestResponse {#WithAndWithoutRequestResponse}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#17)*


### NoRequestNoResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.NoRequestNoResponse}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>



### NoRequestEmptyResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.NoRequestEmptyResponse}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>

### NoRequestWithResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.NoRequestWithResponse}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>ret</code></td>
            <td>
                <code>string</code>
            </td>
        </tr></table>

### WithRequestNoResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.WithRequestNoResponse}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>arg</code></td>
            <td>
                <code>string</code>
            </td>
        </tr></table>



### WithRequestEmptyResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.WithRequestEmptyResponse}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>arg</code></td>
            <td>
                <code>string</code>
            </td>
        </tr></table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>

### WithRequestWithResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.WithRequestWithResponse}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>arg</code></td>
            <td>
                <code>string</code>
            </td>
        </tr></table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>ret</code></td>
            <td>
                <code>string</code>
            </td>
        </tr></table>

### OnEmptyResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.OnEmptyResponse}




#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>

### OnWithResponse {#fidl.test.protocols/WithAndWithoutRequestResponse.OnWithResponse}




#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>ret</code></td>
            <td>
                <code>string</code>
            </td>
        </tr></table>

## WithErrorSyntax {#WithErrorSyntax}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#33)*


### ResponseAsStruct {#fidl.test.protocols/WithErrorSyntax.ResponseAsStruct}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>result</code></td>
            <td>
                <code><a class='link' href='#WithErrorSyntax_ResponseAsStruct_Result'>WithErrorSyntax_ResponseAsStruct_Result</a></code>
            </td>
        </tr></table>

### ErrorAsPrimitive {#fidl.test.protocols/WithErrorSyntax.ErrorAsPrimitive}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>result</code></td>
            <td>
                <code><a class='link' href='#WithErrorSyntax_ErrorAsPrimitive_Result'>WithErrorSyntax_ErrorAsPrimitive_Result</a></code>
            </td>
        </tr></table>

### ErrorAsEnum {#fidl.test.protocols/WithErrorSyntax.ErrorAsEnum}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>result</code></td>
            <td>
                <code><a class='link' href='#WithErrorSyntax_ErrorAsEnum_Result'>WithErrorSyntax_ErrorAsEnum_Result</a></code>
            </td>
        </tr></table>

## ChannelProtocol {#ChannelProtocol}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#40)*


### MethodA {#fidl.test.protocols/ChannelProtocol.MethodA}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>a</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr><tr>
            <td><code>b</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>



### EventA {#fidl.test.protocols/ChannelProtocol.EventA}




#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>a</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr><tr>
            <td><code>b</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>

### MethodB {#fidl.test.protocols/ChannelProtocol.MethodB}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>a</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr><tr>
            <td><code>b</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>result</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>

### MutateSocket {#fidl.test.protocols/ChannelProtocol.MutateSocket}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>a</code></td>
            <td>
                <code>handle&lt;socket&gt;</code>
            </td>
        </tr></table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>b</code></td>
            <td>
                <code>handle&lt;socket&gt;</code>
            </td>
        </tr></table>

## Transitional {#Transitional}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#47)*


### Request {#fidl.test.protocols/Transitional.Request}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>x</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>y</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>

### OneWay {#fidl.test.protocols/Transitional.OneWay}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>x</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>



### Event {#fidl.test.protocols/Transitional.Event}




#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>x</code></td>
            <td>
                <code>int64</code>
            </td>
        </tr></table>



## **STRUCTS**

### WithErrorSyntax_ResponseAsStruct_Response {#WithErrorSyntax_ResponseAsStruct_Response}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#34)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr><tr id="WithErrorSyntax_ResponseAsStruct_Response.a">
            <td><code>a</code></td>
            <td>
                <code>int64</code>
            </td>
            <td></td>
            <td>No default</td>
        </tr><tr id="WithErrorSyntax_ResponseAsStruct_Response.b">
            <td><code>b</code></td>
            <td>
                <code>int64</code>
            </td>
            <td></td>
            <td>No default</td>
        </tr><tr id="WithErrorSyntax_ResponseAsStruct_Response.c">
            <td><code>c</code></td>
            <td>
                <code>int64</code>
            </td>
            <td></td>
            <td>No default</td>
        </tr>
</table>

### WithErrorSyntax_ErrorAsPrimitive_Response {#WithErrorSyntax_ErrorAsPrimitive_Response}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#35)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr>
</table>

### WithErrorSyntax_ErrorAsEnum_Response {#WithErrorSyntax_ErrorAsEnum_Response}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#36)*



<table>
    <tr><th>Name</th><th>Type</th><th>Description</th><th>Default</th></tr>
</table>



## **ENUMS**

### obj_type {#obj_type}
Type: <code>uint32</code>

*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#6)*



<table>
    <tr><th>Name</th><th>Value</th><th>Description</th></tr><tr id="obj_type.NONE">
            <td><code>NONE</code></td>
            <td><code>0</code></td>
            <td></td>
        </tr><tr id="obj_type.SOCKET">
            <td><code>SOCKET</code></td>
            <td><code>14</code></td>
            <td></td>
        </tr></table>

### ErrorEnun {#ErrorEnun}
Type: <code>uint32</code>

*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#28)*



<table>
    <tr><th>Name</th><th>Value</th><th>Description</th></tr><tr id="ErrorEnun.ERR_FOO">
            <td><code>ERR_FOO</code></td>
            <td><code>1</code></td>
            <td></td>
        </tr><tr id="ErrorEnun.ERR_BAR">
            <td><code>ERR_BAR</code></td>
            <td><code>2</code></td>
            <td></td>
        </tr></table>





## **UNIONS**

### WithErrorSyntax_ResponseAsStruct_Result {#WithErrorSyntax_ResponseAsStruct_Result}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#34)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr id="WithErrorSyntax_ResponseAsStruct_Result.response">
            <td><code>response</code></td>
            <td>
                <code><a class='link' href='#WithErrorSyntax_ResponseAsStruct_Response'>WithErrorSyntax_ResponseAsStruct_Response</a></code>
            </td>
            <td></td>
        </tr><tr id="WithErrorSyntax_ResponseAsStruct_Result.err">
            <td><code>err</code></td>
            <td>
                <code>uint32</code>
            </td>
            <td></td>
        </tr></table>

### WithErrorSyntax_ErrorAsPrimitive_Result {#WithErrorSyntax_ErrorAsPrimitive_Result}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#35)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr id="WithErrorSyntax_ErrorAsPrimitive_Result.response">
            <td><code>response</code></td>
            <td>
                <code><a class='link' href='#WithErrorSyntax_ErrorAsPrimitive_Response'>WithErrorSyntax_ErrorAsPrimitive_Response</a></code>
            </td>
            <td></td>
        </tr><tr id="WithErrorSyntax_ErrorAsPrimitive_Result.err">
            <td><code>err</code></td>
            <td>
                <code>uint32</code>
            </td>
            <td></td>
        </tr></table>

### WithErrorSyntax_ErrorAsEnum_Result {#WithErrorSyntax_ErrorAsEnum_Result}
*Defined in [fidl.test.protocols/protocols.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocols.test.fidl#36)*


<table>
    <tr><th>Name</th><th>Type</th><th>Description</th></tr><tr id="WithErrorSyntax_ErrorAsEnum_Result.response">
            <td><code>response</code></td>
            <td>
                <code><a class='link' href='#WithErrorSyntax_ErrorAsEnum_Response'>WithErrorSyntax_ErrorAsEnum_Response</a></code>
            </td>
            <td></td>
        </tr><tr id="WithErrorSyntax_ErrorAsEnum_Result.err">
            <td><code>err</code></td>
            <td>
                <code><a class='link' href='#ErrorEnun'>ErrorEnun</a></code>
            </td>
            <td></td>
        </tr></table>







