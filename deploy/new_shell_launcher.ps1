param([Parameter(Mandatory)][string]$AccountSid,[Parameter(Mandatory)][string]$HostExe,
      [Parameter(Mandatory)][string]$OutputFile)
$ErrorActionPreference='Stop'
$sid=New-Object Security.Principal.SecurityIdentifier($AccountSid)
if (!$sid.IsAccountSid()) {throw 'Specify a user account SID.'}
$path=[Security.SecurityElement]::Escape((Get-Item -LiteralPath $HostExe).FullName)
$xml=@"
<ShellLauncherConfiguration xmlns="http://schemas.microsoft.com/ShellLauncher/2018/Configuration" xmlns:v2="http://schemas.microsoft.com/ShellLauncher/2019/Configuration">
  <Profiles><Profile Id="{9DFF8E16-BB61-4D19-8E9F-19570EF3A9F3}">
    <Shell Shell="&quot;$path&quot;" v2:AppType="Desktop"><DefaultAction Action="DoNothing"/></Shell>
  </Profile></Profiles>
  <Configs><Config><Account Sid="$sid"/><Profile Id="{9DFF8E16-BB61-4D19-8E9F-19570EF3A9F3}"/></Config></Configs>
</ShellLauncherConfiguration>
"@
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputFile),$xml)

