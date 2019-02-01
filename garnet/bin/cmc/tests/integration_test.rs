use cm_fidl;
use failure::Error;
use fidl_fuchsia_data as fd;
use fidl_fuchsia_sys2::{
    CapabilityType, ChildDecl, ComponentDecl, ExposeDecl, OfferDecl, OfferTarget, Relation,
    RelativeId, UseDecl,
};
use std::fs::File;
use std::io::Read;
use std::path::PathBuf;

fn main() {
    let cm_content = read_cm("/pkg/meta/example.cm").expect("could not open example.cm");
    let golden_cm = read_cm("/pkg/data/golden.cm").expect("could not open golden.cm");
    assert_eq!(&cm_content, &golden_cm);

    let cm_decl = cm_fidl::translate(&cm_content).expect("could not translate cm");
    let expected_decl = {
        let program = fd::Dictionary{entries: vec![
            fd::Entry{
                key: "binary".to_string(),
                value: Some(Box::new(fd::Value::Str("bin/example".to_string()))),
            },
        ]};
        let uses = vec![
            UseDecl{
                type_: Some(CapabilityType::Service),
                source_path: Some("/fonts/CoolFonts".to_string()),
                target_path: Some("/svc/fuchsia.fonts.Provider".to_string()),
            },
        ];
        let exposes = vec![
            ExposeDecl{
                type_: Some(CapabilityType::Directory),
                source_path: Some("/volumes/blobfs".to_string()),
                source: Some(RelativeId{
                    relation: Some(Relation::Myself),
                    child_name: None,
                }),
                target_path: Some("/volumes/blobfs".to_string()),
            },
        ];
        let offers = vec![
            OfferDecl{
                type_: Some(CapabilityType::Service),
                source_path: Some("/svc/fuchsia.logger.Log".to_string()),
                source: Some(RelativeId{
                    relation: Some(Relation::Child),
                    child_name: Some("logger".to_string()),
                }),
                targets: Some(vec![
                    OfferTarget{
                        target_path: Some("/svc/fuchsia.logger.Log".to_string()),
                        child_name: Some("netstack".to_string()),
                    },
                ]),
            },
        ];
        let children = vec![
            ChildDecl{
                name: Some("logger".to_string()),
                uri: Some("fuchsia-pkg://fuchsia.com/logger/stable#meta/logger.cm".to_string()),
            },
            ChildDecl{
                name: Some("netstack".to_string()),
                uri: Some("fuchsia-pkg://fuchsia.com/netstack/stable#meta/netstack.cm".to_string()),
            },
        ];
        let facets = fd::Dictionary{entries: vec![
            fd::Entry{
                key: "author".to_string(),
                value: Some(Box::new(fd::Value::Str("Fuchsia".to_string()))),
            },
            fd::Entry{
                key: "year".to_string(),
                value: Some(Box::new(fd::Value::Fnum(2018.))),
            },
        ]};
        ComponentDecl{
            program: Some(program),
            uses: Some(uses),
            exposes: Some(exposes),
            offers: Some(offers),
            children: Some(children),
            facets: Some(facets)
        }
    };
    assert_eq!(cm_decl, expected_decl);
}

fn read_cm(file: &str) -> Result<String, Error> {
    let mut buffer = String::new();
    let path = PathBuf::from(file);
    File::open(&path)?.read_to_string(&mut buffer)?;
    Ok(buffer)
}
