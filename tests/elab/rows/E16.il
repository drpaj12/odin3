# generated: elab-check --write
attribute \top 1
module \e16_indexed_part
  wire width 8 input 1 \a
  wire width 3 input 2 \s
  wire width 2 input 3 \d
  wire width 4 output 4 \y1
  wire width 4 output 5 \y2
  wire width 4 output 6 \y3
  wire width 8 output 7 \r
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
  cell $shiftx $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \s
    connect \Y \y1
  end
  cell $sub $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \s
    connect \B 32'00000000000000000000000000000011
    connect \Y { $n67 $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 }
  end
  cell $shiftx $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 1
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n67 $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 }
    connect \Y \y2
  end
  process $proc$E16_indexed_part_select.v:6$13
    assign { $n78 $n77 $n76 } \s
    assign { $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 } { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 }
    switch { $n78 $n77 $n76 }
      case 3'000
        assign { $n86 $n85 $n84 $n83 $n82 $n81 } 6'000000
        assign { $n80 $n79 } \d
      case 3'001
        assign { $n86 $n85 $n84 $n83 $n82 $n79 } 6'000000
        assign { $n81 $n80 } \d
      case 3'010
        assign { $n86 $n85 $n84 $n83 $n80 $n79 } 6'000000
        assign { $n82 $n81 } \d
      case 3'011
        assign { $n86 $n85 $n84 $n81 $n80 $n79 } 6'000000
        assign { $n83 $n82 } \d
      case 3'100
        assign { $n86 $n85 $n82 $n81 $n80 $n79 } 6'000000
        assign { $n84 $n83 } \d
      case 3'101
        assign { $n86 $n83 $n82 $n81 $n80 $n79 } 6'000000
        assign { $n85 $n84 } \d
      case 3'110
        assign { $n84 $n83 $n82 $n81 $n80 $n79 } 6'000000
        assign { $n86 $n85 } \d
      case 3'111
        assign { $n85 $n84 $n83 $n82 $n81 $n80 $n79 } 7'0000000
        assign $n86 \d [0]
      case
        assign { $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 } 8'00000000
    end
    sync always
      update \r { $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 }
  end
  connect \y3 \a [3:0]
end
