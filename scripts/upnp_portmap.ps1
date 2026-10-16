<#
.SYNOPSIS
  UPnP (IGD) port-mapping helpers for KenshiCoop host sessions. Dot-source
  this file, then call Add-UpnpMapping / Remove-UpnpMapping / Test-UpnpHairpin.

.DESCRIPTION
  Two strategies, tried in order:
    1. The built-in Windows COM object HNetCfg.NATUPnP - instant when it
       works, but Windows disables its discovery on "Public" network
       profiles (very common on friends' machines).
    2. Direct SSDP discovery + SOAP calls to the router's WANIPConnection /
       WANPPPConnection service - pure PowerShell sockets/HTTP, works
       regardless of the Windows network profile.
  No admin rights, no extra binaries; ships in the remote kit as a plain
  script. Acquisition returns Ok/Owned/Unresolved and the original target
  receipt. Unresolved attempted mutations and owned leases require cleanup;
  cleanup throws unless absence on that original target is established.
#>

function Get-LanIPv4 {
    # The local address the router should forward to. Routed-socket trick:
    # "connecting" a UDP socket toward a public address sends no packet but
    # makes the OS pick the outbound interface, whose address we read back.
    # Works without the NetTCPIP module.
    try {
        $u = New-Object System.Net.Sockets.UdpClient
        try {
            $u.Connect("8.8.8.8", 53)
            return $u.Client.LocalEndPoint.Address.ToString()
        } finally { $u.Close() }
    } catch { return $null }
}

# ---- Strategy 2 internals: SSDP + SOAP -------------------------------------------

function Find-UpnpIgd {
    # SSDP M-SEARCH for an Internet Gateway Device, then fetch each announced
    # device description and look for a WANIPConnection/WANPPPConnection
    # service. Returns @{ ControlUrl; ServiceType } or $null.
    $locations = @()
    try {
        $udp = New-Object System.Net.Sockets.UdpClient
        try {
            $udp.Client.ReceiveTimeout = 1000
            foreach ($st in @("urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                              "urn:schemas-upnp-org:device:InternetGatewayDevice:2")) {
                $msg = @(
                    "M-SEARCH * HTTP/1.1",
                    "HOST: 239.255.255.250:1900",
                    "MAN: `"ssdp:discover`"",
                    "MX: 2",
                    "ST: $st",
                    "", ""
                ) -join "`r`n"
                $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
                [void]$udp.Send($bytes, $bytes.Length, "239.255.255.250", 1900)
            }
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $deadline = (Get-Date).AddSeconds(3)
            while ((Get-Date) -lt $deadline) {
                try {
                    $data = $udp.Receive([ref]$remote)
                    $text = [System.Text.Encoding]::ASCII.GetString($data)
                    if ($text -match "(?im)^LOCATION:\s*(\S+)") {
                        $loc = $Matches[1].Trim()
                        if ($locations -notcontains $loc) { $locations += $loc }
                    }
                } catch { }  # receive timeout; keep waiting until the deadline
            }
        } finally { $udp.Close() }
    } catch { return $null }

    foreach ($loc in $locations) {
        try {
            $resp = Invoke-WebRequest -Uri $loc -UseBasicParsing -TimeoutSec 5
            $xml = [xml]$resp.Content
            $base = New-Object System.Uri($loc)
            foreach ($svc in $xml.SelectNodes("//*[local-name()='service']")) {
                $type = "$($svc.serviceType)"
                if ($type -match "WAN(IP|PPP)Connection") {
                    $ctl = New-Object System.Uri($base, "$($svc.controlURL)")
                    return @{ ControlUrl = $ctl.AbsoluteUri; ServiceType = $type }
                }
            }
        } catch { }
    }
    return $null
}

function Invoke-UpnpSoap {
    # POST one SOAP action to the IGD control URL. Returns the response [xml].
    # Throws with a readable message on SOAP faults (UPnP errorCode included).
    param(
        [string]$ControlUrl,
        [string]$ServiceType,
        [string]$Action,
        [System.Collections.Specialized.OrderedDictionary]$Arguments
    )
    $argXml = ""
    if ($null -ne $Arguments) {
        foreach ($k in $Arguments.Keys) {
            $v = [System.Security.SecurityElement]::Escape("$($Arguments[$k])")
            $argXml += "<$k>$v</$k>"
        }
    }
    $body = "<?xml version=`"1.0`"?>" +
        "<s:Envelope xmlns:s=`"http://schemas.xmlsoap.org/soap/envelope/`" " +
        "s:encodingStyle=`"http://schemas.xmlsoap.org/soap/encoding/`">" +
        "<s:Body><u:$Action xmlns:u=`"$ServiceType`">$argXml</u:$Action></s:Body></s:Envelope>"
    try {
        $resp = Invoke-WebRequest -Uri $ControlUrl -Method Post -UseBasicParsing -TimeoutSec 10 `
            -ContentType 'text/xml; charset="utf-8"' `
            -Headers @{ "SOAPAction" = "`"$ServiceType#$Action`"" } `
            -Body $body
        return [xml]$resp.Content
    } catch {
        $failure=$_
        $detail=$failure.Exception.Message;$code=$null
        try {
            $response=$failure.Exception.Response
            if ($response -and $response.PSObject.Methods['GetResponseStream']) {
                $reader=New-Object IO.StreamReader($response.GetResponseStream())
                try {$errBody=$reader.ReadToEnd()} finally {$reader.Dispose()}
            } elseif ($response -and $response.Content) {
                $errBody=$response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
            } else {$errBody=''}
            if ($errBody) {
                $fault=[xml]$errBody
                $errorNode=$fault.SelectSingleNode("//*[local-name()='errorCode']")
                $descriptionNode=$fault.SelectSingleNode("//*[local-name()='errorDescription']")
                if ($errorNode) {$code=[int]$errorNode.InnerText;$detail="UPnP error $code"}
                if ($descriptionNode) {$detail+=" ($($descriptionNode.InnerText))"}
            }
        } catch {}
        $errorObject=New-Object InvalidOperationException("$Action failed: $detail",$failure.Exception)
        if ($null -ne $code) {$errorObject.Data['UPnPErrorCode']=$code}
        throw $errorObject
    }
}

# ---- Public API --------------------------------------------------------------------

function Get-UpnpSoapMapping {
    param($Igd,[int]$Port)
    $args=New-Object Collections.Specialized.OrderedDictionary
    $args['NewRemoteHost']='';$args['NewExternalPort']=$Port;$args['NewProtocol']='UDP'
    try {
        $query=Invoke-UpnpSoap -ControlUrl $Igd.ControlUrl -ServiceType $Igd.ServiceType -Action 'GetSpecificPortMappingEntry' -Arguments $args
    } catch {
        if ($_.Exception.Data['UPnPErrorCode'] -eq 714) {return $null}
        throw
    }
    $values=@{}
    foreach($name in @('NewInternalClient','NewInternalPort','NewEnabled','NewPortMappingDescription')) {
        $node=$query.SelectSingleNode("//*[local-name()='$name']")
        if (-not $node) {throw "Incomplete UPnP mapping response: $name"}
        $values[$name]=$node.InnerText
    }
    $enabled = switch ($values.NewEnabled.Trim().ToLowerInvariant()) {
        { $_ -in @('1','true','yes') } { $true; break }
        { $_ -in @('0','false','no') } { $false; break }
        default { throw "Malformed UPnP enabled value: $($values.NewEnabled)" }
    }
    return [pscustomobject]@{
        ExternalPort=$Port;Protocol='UDP';InternalClient=$values.NewInternalClient
        InternalPort=[int]$values.NewInternalPort;Enabled=$enabled
        Description=$values.NewPortMappingDescription
    }
}

function Test-UpnpMappingIdentity {
    param($Entry,$Mapping)
    return ($null -ne $Entry -and
        [int]$Entry.ExternalPort -eq [int]$Mapping.Port -and
        [string]$Entry.Protocol -ieq [string]$Mapping.Protocol -and
        [string]$Entry.InternalClient -ceq [string]$Mapping.InternalClient -and
        [int]$Entry.InternalPort -eq [int]$Mapping.InternalPort -and
        [bool]$Entry.Enabled -and [string]$Entry.Description -ceq [string]$Mapping.Description)
}

function Add-UpnpMapping {
    param([Parameter(Mandatory=$true)][ValidateRange(1,65535)][int]$Port,[string]$Description='KenshiCoop')
    $result=[pscustomobject]@{
        Ok=$false;Owned=$false;Unresolved=$false;ExternalIp=$null;LanIp=$null;Error=''
        Port=$Port;Protocol='UDP';InternalPort=$Port;InternalClient=$null
        Description=("$Description ["+[Guid]::NewGuid().ToString('N')+']')
        Method='';ControlUrl='';ServiceType='';Collection=$null
    }
    $lanIp=Get-LanIPv4
    if (-not $lanIp) {$result.Error="could not determine this PC's LAN IP";return $result}
    $result.LanIp=$lanIp;$result.InternalClient=$lanIp
    try {
        $nat=New-Object -ComObject HNetCfg.NATUPnP
        $collection=$nat.StaticPortMappingCollection
        if ($null -ne $collection) {
            $existing=@($collection | Where-Object {$_.ExternalPort -eq $Port -and $_.Protocol -ieq 'UDP'} | Select-Object -First 1)
            if ($existing.Count) {
                $entry=$existing[0]
                if ($entry.InternalClient -cne $lanIp -or $entry.InternalPort -ne $Port -or -not $entry.Enabled) {
                    $result.Error='UDP port already maps to a different or disabled destination; not overwritten.'
                    return $result
                }
                $result.Ok=$true;$result.Description=$entry.Description;$result.Method='com'
                $result.ExternalIp=$entry.ExternalIPAddress
                return $result
            }
            $result.Method='com';$result.Collection=$collection
            $result.Unresolved=$true
            [void]$collection.Add($Port,'UDP',$Port,$lanIp,$true,$result.Description)
            $entry=$collection.Item($Port,'UDP')
            if (-not (Test-UpnpMappingIdentity $entry $result)) {throw 'Created router mapping could not be verified.'}
            $result.Owned=$true;$result.Unresolved=$false
            $result.Ok=$true;$result.ExternalIp=$entry.ExternalIPAddress
            return $result
        }
    } catch {
        if ($result.Owned) {$result.Error="UPnP metadata query failed after verified acquisition: $_";return $result}
        if ($result.Unresolved) {
            $result.Error="UPnP Add response/verification failed: $_"
            # Never discover a different router after an attempted mutation.
            try {
                $entries=@($result.Collection | Where-Object {$_.ExternalPort -eq $Port -and $_.Protocol -ieq 'UDP'})
                if (-not $entries.Count) { $result.Unresolved=$false }
                elseif ($entries.Count -eq 1 -and (Test-UpnpMappingIdentity $entries[0] $result)) {
                    $result.Owned=$true;$result.Unresolved=$false;$result.Ok=$true
                    $result.ExternalIp=$entries[0].ExternalIPAddress;$result.Error=''
                }
            } catch { $result.Error+="; original-router ownership reconciliation unavailable: $_" }
            return $result
        }
    }
    $igd=Find-UpnpIgd
    if ($null -eq $igd) {$result.Error='Router did not answer UPnP discovery.';return $result}
    $result.Method='soap';$result.ControlUrl=$igd.ControlUrl;$result.ServiceType=$igd.ServiceType
    try {
        $existing=Get-UpnpSoapMapping $igd $Port
        if ($existing) {
            if ($existing.InternalClient -cne $lanIp -or $existing.InternalPort -ne $Port -or -not $existing.Enabled) {
                $result.Error='UDP port already maps to a different or disabled destination; not overwritten.'
                return $result
            }
            $result.Description=$existing.Description
        } else {
            $args=New-Object Collections.Specialized.OrderedDictionary
            $args['NewRemoteHost']='';$args['NewExternalPort']=$Port;$args['NewProtocol']='UDP'
            $args['NewInternalPort']=$Port;$args['NewInternalClient']=$lanIp;$args['NewEnabled']=1
            $args['NewPortMappingDescription']=$result.Description;$args['NewLeaseDuration']=0
            $result.Unresolved=$true
            [void](Invoke-UpnpSoap -ControlUrl $igd.ControlUrl -ServiceType $igd.ServiceType -Action 'AddPortMapping' -Arguments $args)
            if (-not (Test-UpnpMappingIdentity (Get-UpnpSoapMapping $igd $Port) $result)) {throw 'Created router mapping could not be verified.'}
            $result.Owned=$true;$result.Unresolved=$false
        }
        $result.Ok=$true
        try {
            $external=Invoke-UpnpSoap -ControlUrl $igd.ControlUrl -ServiceType $igd.ServiceType -Action 'GetExternalIPAddress' -Arguments $null
            $node=$external.SelectSingleNode("//*[local-name()='NewExternalIPAddress']")
            if ($node) {$result.ExternalIp=$node.InnerText}
        } catch {}
    } catch {
        $result.Error="UPnP request failed: $_"
        if ($result.Unresolved) {
            try {
                $entry=Get-UpnpSoapMapping $igd $Port
                if (-not $entry) { $result.Unresolved=$false }
                elseif (Test-UpnpMappingIdentity $entry $result) {
                    $result.Owned=$true;$result.Unresolved=$false;$result.Ok=$true;$result.Error=''
                }
            } catch { $result.Error+="; original-router ownership reconciliation unavailable: $_" }
        }
    }
    return $result
}

function Remove-UpnpMapping {
    param([Parameter(Mandatory=$true)]$Mapping)
    if (-not ($Mapping.Owned -or $Mapping.Unresolved)) {return}
    try {
        if ($Mapping.Method -eq 'com') {
            $collection=$Mapping.Collection
            if ($null -eq $collection) {throw 'The original router mapping collection is unavailable.'}
            $entries=@($collection | Where-Object {$_.ExternalPort -eq $Mapping.Port -and $_.Protocol -ieq $Mapping.Protocol})
            if (-not $entries.Count) {$Mapping.Unresolved=$false;return}
            if ($entries.Count -ne 1 -or -not (Test-UpnpMappingIdentity $entries[0] $Mapping)) {throw 'Mapping changed ownership; it was not removed.'}
            $collection.Remove($Mapping.Port,$Mapping.Protocol)
            $remaining=@($collection | Where-Object {$_.ExternalPort -eq $Mapping.Port -and $_.Protocol -ieq $Mapping.Protocol})
            if ($remaining.Count) {throw 'Original router still reports the mapping after deletion.'}
        } elseif ($Mapping.Method -eq 'soap') {
            $igd=@{ControlUrl=$Mapping.ControlUrl;ServiceType=$Mapping.ServiceType}
            $entry=Get-UpnpSoapMapping $igd $Mapping.Port
            if (-not $entry) {$Mapping.Unresolved=$false;return}
            if (-not (Test-UpnpMappingIdentity $entry $Mapping)) {throw 'Mapping changed ownership; it was not removed.'}
            $args=New-Object Collections.Specialized.OrderedDictionary
            $args['NewRemoteHost']='';$args['NewExternalPort']=$Mapping.Port;$args['NewProtocol']=$Mapping.Protocol
            try {
                [void](Invoke-UpnpSoap -ControlUrl $Mapping.ControlUrl -ServiceType $Mapping.ServiceType -Action 'DeletePortMapping' -Arguments $args)
            } catch {
                if ($_.Exception.Data['UPnPErrorCode'] -ne 714) {throw}
            }
            if (Get-UpnpSoapMapping $igd $Mapping.Port) {throw 'Original router still reports the mapping after deletion.'}
        } else {throw 'Unknown mapping ownership method; it was not removed.'}
        $Mapping.Unresolved=$false
    } catch {throw "Owned/unresolved UPnP mapping cleanup was not completed on the original router: $_"}
}

function Test-UpnpHairpin {
    # Best-effort external-reachability probe: listen on UDP $Port, fire a
    # datagram at our own public IP:$Port, and see if the router loops it back
    # through the fresh mapping (NAT hairpinning). Many consumer routers don't
    # hairpin at all, so the only trustworthy answers are "reachable" and
    # "inconclusive" - never treat this probe alone as a failure.
    # Call BEFORE the game launches (the game needs the port).
    param(
        [Parameter(Mandatory = $true)][int]$Port,
        [Parameter(Mandatory = $true)][string]$ExternalIp
    )
    $listener = $null
    $sender = $null
    try {
        $token = "KenshiCoopHairpin_" + [Guid]::NewGuid().ToString("N")
        $payload = [System.Text.Encoding]::ASCII.GetBytes($token)
        $listener = New-Object System.Net.Sockets.UdpClient($Port)
        $listener.Client.ReceiveTimeout = 2500
        $sender = New-Object System.Net.Sockets.UdpClient
        [void]$sender.Send($payload, $payload.Length, $ExternalIp, $Port)
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $deadline = (Get-Date).AddSeconds(3)
        while ((Get-Date) -lt $deadline) {
            try {
                $data = $listener.Receive([ref]$remote)
                if ([System.Text.Encoding]::ASCII.GetString($data) -eq $token) { return "reachable" }
            } catch { break }  # receive timeout
        }
        return "inconclusive"
    } catch {
        return "inconclusive"
    } finally {
        if ($null -ne $sender)   { $sender.Close() }
        if ($null -ne $listener) { $listener.Close() }
    }
}
