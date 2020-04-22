pub fn my_function() -> u8 {
    8u8 + 3u8
}

pub fn set_args(op: Option<String>) -> Option<String> {
    if op.is_some() {
        Some("".to_string())
    } else {
        None
    }
}

#[cfg(target_arch = "wasm32")]
mod bindings {
    use wasm_bindgen::prelude::*;
    #[wasm_bindgen]
    pub extern "C" fn my_function() -> u8 {
        super::my_function()
    }

    #[wasm_bindgen]
    pub extern "C" fn set_args(op: Option<String>) -> Option<String> {
        super::set_args(op)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn my_function_test() {
        assert_eq!(11, my_function());
    }

    #[test]
    fn set_args_test() {
        assert_eq!(None, set_args(None));
        assert_eq!(Some("".to_string()), set_args(Some("hello".to_string())));
    }
}
