# Sets the tree's LED output mode (diagnostic, not persisted): 0 = phased DMA (normal),
# 1 = DMA one string at a time, 2 = CPU writes with interrupts off (the original method),
# 3 = CPU writes with interrupts on. See docs/LED_OUTPUT.md.
param([Parameter(Mandatory = $true)][int]$Mode, [string]$TreeHost = "neotree.local")
$c = New-Object System.Net.Sockets.TcpClient; $c.NoDelay = $true; $c.Connect($TreeHost, 7777)
$s = $c.GetStream(); $s.ReadTimeout = 3000
function Read-Frame { $h = New-Object byte[] 2; [void]$s.Read($h, 0, 2); $n = $h[0] + 256 * $h[1]; $b = New-Object byte[] $n; $got = 0; while ($got -lt $n) { $got += $s.Read($b, $got, $n - $got) }; return ,$b }
[void](Read-Frame)
$msg = [byte[]](2, 0, 16, $Mode); $s.Write($msg, 0, 4)
$ack = Read-Frame
Write-Output "mode $Mode -> ack status $($ack[2]) (0 = queued)"
$c.Close()
