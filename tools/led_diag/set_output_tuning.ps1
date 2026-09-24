# Sets the tree's LED output tuning (diagnostic, not persisted): pin slew (0 slow / 1 fast),
# drive (0-3 = 2/4/8/12mA), start stagger between strings in a phase (ns), frame rate target
# (0 = unchanged) and the phase of each string (default 0,0,1,1 = strings 1+2 then 3+4).
# See docs/LED_OUTPUT.md.
param([int]$Slew = 0, [int]$Drive = 1, [int]$StaggerNs = 0, [int]$Fps = 0, [int[]]$Phases = @(0, 0, 1, 1),
      [string]$TreeHost = "neotree.local")
$c = New-Object System.Net.Sockets.TcpClient; $c.NoDelay = $true; $c.Connect($TreeHost, 7777)
$s = $c.GetStream(); $s.ReadTimeout = 3000
function Read-Frame { $h = New-Object byte[] 2; [void]$s.Read($h, 0, 2); $n = $h[0] + 256 * $h[1]; $b = New-Object byte[] $n; $got = 0; while ($got -lt $n) { $got += $s.Read($b, $got, $n - $got) }; return ,$b }
[void](Read-Frame)
$map = $Phases[0] + 4 * $Phases[1] + 16 * $Phases[2] + 64 * $Phases[3]
$msg = [byte[]](6, 0, 17, $Slew, $Drive, [int]($StaggerNs / 10), $Fps, $map); $s.Write($msg, 0, $msg.Length)
$ack = Read-Frame
Write-Output "tuning slew=$Slew drive=$Drive stagger=${StaggerNs}ns fps=$Fps phases=$($Phases -join ",") -> ack status $($ack[2]) (0 = queued)"
$c.Close()
