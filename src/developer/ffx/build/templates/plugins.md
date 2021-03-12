pub async fn ffx_plugin_impl<D, DFut, R, RFut, E, EFut, F, FFut>(
{% if not includes_execution and not includes_subcommands %}
  _daemon_factory: D,
  _remote_factory: R,
  _fastboot_factory: F,
  _is_experiment: E,
  _cmd: {{suite_args_lib}}::FfxPluginCommand,
{% else %}
  daemon_factory: D,
  remote_factory: R,
  fastboot_factory: F,
  is_experiment: E,
  cmd: {{suite_args_lib}}::FfxPluginCommand,
{% endif %}
) -> Result<(), anyhow::Error>
    where
    D: Fn() -> DFut,
    DFut: std::future::Future<
        Output = anyhow::Result<fidl_fuchsia_developer_bridge::DaemonProxy>,
    >,
    R: Fn() -> RFut,
    RFut: std::future::Future<
        Output = anyhow::Result<
            fidl_fuchsia_developer_remotecontrol::RemoteControlProxy,
        >,
    >,
    F: Fn() -> FFut,
    FFut: std::future::Future<
        Output = anyhow::Result<fidl_fuchsia_developer_bridge::FastbootProxy>,
    >,
    E: Fn(&'static str) -> EFut,
    EFut: std::future::Future<Output = bool>,
{
{% if includes_execution %}
{% if includes_subcommands %}
  match cmd.subcommand {
      Some(sub) => match sub {
{% for plugin in plugins %}
        {{suite_subcommand_lib}}::Subcommand::{{plugin.enum}}(c) => {{plugin.lib}}_suite::ffx_plugin_impl(daemon_factory, remote_factory, fastboot_factory, is_experiment, c).await,
{% endfor %}
      },
      None => {{execution_lib}}::ffx_plugin_impl(daemon_factory, remote_factory, fastboot_factory, is_experiment, cmd).await
    }
{% else %}
  {{execution_lib}}::ffx_plugin_impl(daemon_factory, remote_factory, fastboot_factory, is_experiment, cmd).await
{% endif %}

{% else %}
{% if includes_subcommands %}
    match cmd.subcommand {
{% for plugin in plugins %}
      {{suite_subcommand_lib}}::Subcommand::{{plugin.enum}}(c) => {{plugin.lib}}_suite::ffx_plugin_impl(daemon_factory, remote_factory, fastboot_factory, is_experiment, c).await,
{% endfor %}
    }
{% else %}
    println!("Subcommand not implemented yet.");
    Ok(())
{% endif %}
{% endif %}
}
