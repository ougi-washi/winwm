[CmdletBinding()]
param([Parameter(Mandatory)][string]$AccountSid,
      [Parameter(Mandatory)][string]$OutputFile,
      [ValidateSet('AuditOnly','Enabled')][string]$Mode='AuditOnly')
$ErrorActionPreference='Stop'
$sid=New-Object Security.Principal.SecurityIdentifier($AccountSid)
if (!$sid.IsAccountSid()) {throw 'Specify a user account SID.'}
# Generate a complete deployment policy only for a machine with no executable
# collection. Existing managed collections must retain their administrator's rules.
$policy=[xml](Get-AppLockerPolicy -Effective -Xml)
$exe=@($policy.AppLockerPolicy.RuleCollection|Where-Object Type -eq 'Exe')
if ($exe.Count -and @($exe[0].ChildNodes|Where-Object NodeType -eq Element).Count) {
    throw 'An executable rule collection already exists. Merge a user-specific Windows Explorer deny rule through the existing policy owner.'
}
# Preserve the previous unrestricted executable default for all users. Only the
# explicit account denial changes behavior. Output is reviewed and deployed separately.
$appx=''
$packaged=@($policy.AppLockerPolicy.RuleCollection|Where-Object Type -eq 'Appx')
if (!$packaged.Count -or !@($packaged[0].ChildNodes|Where-Object NodeType -eq Element).Count) {
    $appx='<RuleCollection Type="Appx" EnforcementMode="Enabled"><FilePublisherRule Id="e9355c2d-b171-4391-9751-f5f01b8c8646" Name="Preserve signed packaged application access" Description="" UserOrGroupSid="S-1-1-0" Action="Allow"><Conditions><FilePublisherCondition PublisherName="*" ProductName="*" BinaryName="*"><BinaryVersionRange LowSection="0.0.0.0" HighSection="*"/></FilePublisherCondition></Conditions></FilePublisherRule></RuleCollection>'
}
$xml=@"
<AppLockerPolicy Version="1">
  <RuleCollection Type="Exe" EnforcementMode="$Mode">
    <FilePathRule Id="5d26e6f9-3bff-4f24-997a-136cd0c6bb19" Name="Preserve existing executable access" Description="" UserOrGroupSid="S-1-1-0" Action="Allow">
      <Conditions><FilePathCondition Path="*"/></Conditions>
    </FilePathRule>
    <FilePathRule Id="9b06d297-f67d-4430-8cf1-108003b117ac" Name="Exclude Windows Explorer from the desktop account" Description="" UserOrGroupSid="$sid" Action="Deny">
      <Conditions><FilePathCondition Path="%WINDIR%\explorer.exe"/></Conditions>
    </FilePathRule>
  </RuleCollection>
$appx
</AppLockerPolicy>
"@
[IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputFile),$xml)

