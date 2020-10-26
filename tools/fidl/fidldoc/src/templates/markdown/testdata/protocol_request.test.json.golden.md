[TOC]

# fidl.test.protocolrequest


## **PROTOCOLS**

## Child {#Child}
*Defined in [fidl.test.protocolrequest/protocol_request.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocol_request.test.fidl#3)*


## Parent {#Parent}
*Defined in [fidl.test.protocolrequest/protocol_request.test.fidl](https://fuchsia.googlesource.com/fuchsia/+/HEAD/protocol_request.test.fidl#6)*


### GetChild {#fidl.test.protocolrequest/Parent.GetChild}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>c</code></td>
            <td>
                <code><a class='link' href='#Child'>Child</a></code>
            </td>
        </tr></table>

### GetChildRequest {#fidl.test.protocolrequest/Parent.GetChildRequest}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    </table>


#### Response
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>r</code></td>
            <td>
                <code>request&lt;<a class='link' href='#Child'>Child</a>&gt;</code>
            </td>
        </tr></table>

### TakeChild {#fidl.test.protocolrequest/Parent.TakeChild}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>c</code></td>
            <td>
                <code><a class='link' href='#Child'>Child</a></code>
            </td>
        </tr></table>



### TakeChildRequest {#fidl.test.protocolrequest/Parent.TakeChildRequest}


#### Request
<table>
    <tr><th>Name</th><th>Type</th></tr>
    <tr>
            <td><code>r</code></td>
            <td>
                <code>request&lt;<a class='link' href='#Child'>Child</a>&gt;</code>
            </td>
        </tr></table>





## **STRUCTS**













