# generated: elab-check --write
attribute \top 1
module \k3_func_ret
  wire width 4 input 1 \a
  wire width 4 output 2 \y1
  wire width 8 output 3 signed \y2
  wire width 32 output 4 \y3
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
  cell $not $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \Y { $n68 $n67 $n66 $n65 }
  end
  process $proc$K3_function_return_types.v:24$6
    assign { $n56 $n55 $n54 $n53 } \a
    assign { $n60 $n59 $n58 $n57 } { $n68 $n67 $n66 $n65 }
    assign { $n64 $n63 $n62 $n61 } { $n68 $n67 $n66 $n65 }
    sync always
      update { $n52 $n51 $n50 $n49 } { $n64 $n63 $n62 $n61 }
  end
  process $proc$K3_function_return_types.v:25$7
    assign { $n80 $n79 $n78 $n77 } \a
    assign { $n88 $n87 $n86 $n85 $n84 $n83 $n82 $n81 } { \a [3] \a [3] \a [3] \a [3] \a }
    assign { $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 } { \a [3] \a [3] \a [3] \a [3] \a }
    sync always
      update { $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 } { $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 }
  end
  connect \y1 { $n52 $n51 $n50 $n49 }
  connect \y2 { $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 }
  connect \y3 32'00000000000000000000000000000010
end
