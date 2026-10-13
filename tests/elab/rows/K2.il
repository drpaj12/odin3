# generated: elab-check --write
attribute \top 1
module \k2_func_ansi
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire input 3 \s
  wire width 4 output 4 \y1
  wire width 6 output 5 \y2
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n65
  wire $n66
  wire $n67
  wire $n68
  wire $n69
  wire $n70
  wire $n71
  wire $n72
  wire $n73
  wire $n74
  wire $n75
  wire $n76
  wire $n77
  wire $n78
  wire $n79
  wire $n80
  wire $n81
  wire $n82
  wire $n83
  wire $n84
  wire $n85
  wire $n86
  wire $n87
  wire $n88
  wire $n89
  wire $n90
  wire $n91
  wire $n92
  wire $n93
  wire $n94
  wire $n95
  wire $n96
  wire $n97
  wire $n98
  wire $n99
  wire $n100
  wire $n101
  wire $n102
  wire $n103
  wire $n104
  wire $n105
  wire $n106
  wire $n107
  wire $n108
  wire $n109
  wire $n110
  wire $n111
  wire $n112
  wire $n113
  wire $n114
  wire $n115
  wire $n116
  wire $n117
  wire $n118
  wire $n119
  wire $n120
  wire $n121
  wire $n122
  wire $n123
  wire $n124
  wire $n127
  wire $n128
  wire $n129
  wire $n130
  wire $n131
  wire $n132
  cell $mux $c6
    parameter \WIDTH 4
    connect \A \b
    connect \B \a
    connect \S \s
    connect \Y { $n44 $n43 $n42 $n41 }
  end
  cell $shl $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 6
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 6
    connect \A { 2'00 \a }
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n132 $n131 $n130 $n129 $n128 $n127 }
  end
  process $proc$K2_function_ansi.v:17$7
    assign $n24 \s
    assign { $n28 $n27 $n26 $n25 } \a
    assign { $n32 $n31 $n30 $n29 } \b
    assign { $n36 $n35 $n34 $n33 } { $n44 $n43 $n42 $n41 }
    assign { $n40 $n39 $n38 $n37 } { $n44 $n43 $n42 $n41 }
    sync always
      update { $n23 $n22 $n21 $n20 } { $n40 $n39 $n38 $n37 }
  end
  process $proc$K2_function_ansi.v:18$11
    assign { $n54 $n53 $n52 $n51 } \a
    assign { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 } 32'00000000000000000000000000000001
    assign { $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 $n87 } 32'00000000000000000000000000000001
    assign { $n124 $n123 $n122 $n121 $n120 $n119 } { $n132 $n131 $n130 $n129 $n128 $n127 }
    sync always
      update { $n50 $n49 $n48 $n47 $n46 $n45 } { $n124 $n123 $n122 $n121 $n120 $n119 }
  end
  connect \y1 { $n23 $n22 $n21 $n20 }
  connect \y2 { $n50 $n49 $n48 $n47 $n46 $n45 }
end
