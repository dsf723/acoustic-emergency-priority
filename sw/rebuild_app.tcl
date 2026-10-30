# Rebuild application only; preserve BSP and hardware platform.
set here [file dirname [file normalize [info script]]]
foreach source [glob -nocomplain -directory $here/src *] {
    if {[file isfile $source]} {
        file copy -force $source $here/workspace/sound_loc/src/
    }
}
setws $here/workspace
projects -clean -type app -name sound_loc
projects -build -type app -name sound_loc
puts "INFO: application rebuild complete"
