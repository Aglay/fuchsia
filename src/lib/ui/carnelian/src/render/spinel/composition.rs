// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{ptr, slice};

use euclid::Rect;
use spinel_rs_sys::*;

use crate::{
    render::{
        spinel::{init, InnerContext, Spinel},
        BlendMode, Composition, Fill, FillRule, Layer, Style,
    },
    Color,
};

fn group_layers(
    spn_styling: SpnStyling,
    top_group: SpnGroupId,
    layers: &[Layer<Spinel>],
    layer_id_start: u32,
    override_color: Option<[f32; 4]>,
) {
    fn cmds_len(style: &Style) -> usize {
        let fill_rule_len = match style.fill_rule {
            FillRule::NonZero => 1,
            FillRule::EvenOdd => 1,
        };
        let fill_len = match style.fill {
            Fill::Solid(..) => 3,
        };
        let blend_mode_len = match style.blend_mode {
            BlendMode::Over => 1,
        };

        1 + fill_rule_len + fill_len + blend_mode_len
    }

    for (i, Layer { style, .. }) in layers.iter().enumerate() {
        let cmds = unsafe {
            let len = cmds_len(style);
            let data = init(|ptr| {
                spn!(spn_styling_group_layer(
                    spn_styling,
                    top_group,
                    layer_id_start + i as u32,
                    len as u32,
                    ptr
                ))
            });
            slice::from_raw_parts_mut(data, len)
        };

        cmds[0] = SpnCommand::SpnStylingOpcodeCoverWipZero;
        let mut cursor = 1;

        match style.fill_rule {
            FillRule::NonZero => {
                cmds[cursor] = SpnCommand::SpnStylingOpcodeCoverNonzero;
                cursor += 1;
            }
            FillRule::EvenOdd => {
                cmds[cursor] = SpnCommand::SpnStylingOpcodeCoverEvenodd;
                cursor += 1;
            }
        }

        match style.fill {
            Fill::Solid(color) => {
                let color = override_color.unwrap_or(color.to_f32());
                unsafe {
                    spn_styling_layer_fill_rgba_encoder(&mut cmds[cursor], color.as_ptr());
                }
                cursor += 3;
            }
        }

        match style.blend_mode {
            BlendMode::Over => {
                cmds[cursor] = SpnCommand::SpnStylingOpcodeBlendOver;
            }
        }
    }
}

#[derive(Clone, Debug)]
pub struct SpinelComposition {
    layers: Vec<Layer<Spinel>>,
    background_color: [f32; 4],
}

impl SpinelComposition {
    pub(crate) fn set_up_spn_composition(
        &self,
        composition: SpnComposition,
        clip: Rect<u32>,
        clear_composition: Option<&Self>,
    ) {
        unsafe {
            spn!(spn_composition_reset(composition));
            spn!(spn_composition_set_clip(
                composition,
                [clip.origin.x, clip.origin.y, clip.size.width, clip.size.height].as_ptr(),
            ));
        }

        let layers = self.layers.iter().chain(
            clear_composition.map(|composition| composition.layers.as_slice()).unwrap_or(&[]),
        );

        for (i, Layer { raster, .. }) in layers.enumerate() {
            for raster in raster.raster().iter() {
                unsafe {
                    spn!(spn_composition_place(
                        composition,
                        &**raster,
                        &(i as u32),
                        ptr::null(),
                        1,
                    ));
                }
            }
        }
    }

    pub(crate) fn spn_styling(
        &self,
        context: &InnerContext,
        clear_composition: Option<&Self>,
    ) -> Option<SpnStyling> {
        let len = match clear_composition {
            Some(composition) => self.layers.len() + composition.layers.len(),
            None => self.layers.len(),
        };
        let spn_styling = context.get_checked().map(|context| unsafe {
            init(|ptr| spn!(spn_styling_create(context, ptr, len as u32, len as u32 * 8)))
        })?;

        let top_group = unsafe { init(|ptr| spn!(spn_styling_group_alloc(spn_styling, ptr))) };

        unsafe {
            spn!(spn_styling_group_parents(spn_styling, top_group, 0, ptr::null_mut()));
            spn!(spn_styling_group_range_lo(spn_styling, top_group, 0));
            spn!(spn_styling_group_range_lo(spn_styling, top_group, self.layers.len() as u32 - 1));
        }

        let cmds_enter = unsafe {
            let data = init(|ptr| spn!(spn_styling_group_enter(spn_styling, top_group, 1, ptr)));
            slice::from_raw_parts_mut(data, 1)
        };
        cmds_enter[0] = SpnCommand::SpnStylingOpcodeColorAccZero;

        let cmds_leave = unsafe {
            let data = init(|ptr| spn!(spn_styling_group_leave(spn_styling, top_group, 4, ptr)));
            slice::from_raw_parts_mut(data, 4)
        };
        unsafe {
            spn_styling_background_over_encoder(&mut cmds_leave[0], self.background_color.as_ptr());
        }
        cmds_leave[3] = SpnCommand::SpnStylingOpcodeColorAccStoreToSurface;

        group_layers(spn_styling, top_group, &self.layers, 0, None);

        if let Some(clear_composition) = clear_composition {
            group_layers(
                spn_styling,
                top_group,
                &clear_composition.layers,
                self.layers.len() as u32,
                Some(self.background_color),
            );
        }

        unsafe {
            spn!(spn_styling_seal(spn_styling));
        }

        Some(spn_styling)
    }
}

impl Composition<Spinel> for SpinelComposition {
    fn new(layers: impl IntoIterator<Item = Layer<Spinel>>, background_color: Color) -> Self {
        Self { layers: layers.into_iter().collect(), background_color: background_color.to_f32() }
    }
}
