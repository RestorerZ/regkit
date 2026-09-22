param([Parameter(Mandatory)][string[]]$path)

$cert = dir Cert:\CurrentUser\My -CodeSigningCert | ? { $_.FriendlyName -eq "Noverse (nohuto)" -and $_.NotAfter -gt (Get-Date) } | sort NotAfter -Descending | select -First 1
if (!$cert) { Write-Warning "cert not found"; exit 0 }

$signtool = dir "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | sort FullName | select -Last 1
if (!$signtool) { Write-Error "sdk is missing"; exit 1 }

& $signtool.FullName sign /sha1 $cert.Thumbprint /fd SHA256 /tr "http://timestamp.digicert.com" /td SHA256 /d "RegKit" /du "https://github.com/nohuto/regkit" $path

exit $LASTEXITCODE
