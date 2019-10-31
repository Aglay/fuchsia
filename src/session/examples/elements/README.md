## Overview

This directory contains an example implementation of a session that 
instantiates a single element proposer. 

The element proposer connects to the [`ElementManager`]() service offered by the
session, and uses said service to add a [`simple_element`]() to the session.

The `simple_element` then connects to the [`ElementPing`] service exposed to it
by the session, and throug it notifies the session that it has been successfully
instantiated.

## Element Session

The element session is configured to:

  1. Declare a static child: the [`element_proposer`]().
  2. Declare a child collection with a name which matches the one passed to the
  [`ElementManagement`]() library.
  3. Expose the `ElementManager` service.
  4. Expose the `ElementPing` service.
  5. Offer the `ElementManager` service to the `element_proposer` child.
  6. Offer the `ElementPing` service to the child collection mentioned above.

Details of how this is done can be found in the [`element_session.cml`]() file.

Once the session is launched, it exposes the aforementioned services and starts
handling requests.

## Element Proposer

The element proposer is configured to:

  1. Use the `ElementManager` service.
  
Details of how this is done can be found in the [`element_proposer.cml`]() file.

Once the element proposer is started it connects to the `ElementManager` service
and attempts to add an element (`simple_element`) to the session.

## Simple Element

The simple element is configured to:

  1. Use the `ElementPing` service.
  
Details of how this is done can be found in the [`simple_element.cml`]() file.

Once the simple element is started, it will call `ElementPing::Ping()`. The
session will receive the ping and log a confirmation.


## How to Run

To run the example, first edit the session manager cml file to set the element
session's component url as the argument:

```
"args": [ "-s", "fuchsia-pkg://fuchsia.com/element_session#meta/element_session.cm" ],
```

TODO(37028): Update this when sessions can be configured without modifying the
session manager's CML file.

Then run the following commands:

```$sh
fx set core.x64 --with-base=//src/session:all
fx build
fx shell run fuchsia-pkg://fuchsia.com/component_manager_sfw#meta/component_manager_sfw.cmx fuchsia-pkg://fuchsia.com/session_manager#meta/session_manager.cm
```

The last command should output a message stating that the element's ping has
been received.
