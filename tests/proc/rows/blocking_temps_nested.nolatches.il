attribute \top 1
module \t
  wire width 2 input 1 \s
  wire input 2 \u
  wire width 4 input 3 \a
  wire width 4 input 4 \b
  wire width 4 output 5 \y
  wire width 4 output 6 \z
  wire width 4 \t
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
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n65
  wire $n70
  wire $n75
  wire $n76
  wire $n81
  wire $n82
  wire $n83
  wire $n84
  wire $n85
  wire $n86
  cell $and $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n47 $n46 $n45 $n44 }
  end
  cell $or $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n55 $n54 $n53 $n52 }
  end
  cell $add $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n51 $n50 $n49 $n48 }
    connect \B 4'0001
    connect \Y { $n61 $n60 $n59 $n58 }
  end
  cell $not $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \Y { $n65 $n64 $n63 $n62 }
  end
  cell $xor $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \t
    connect \B \y
    connect \Y \z
  end
  attribute \full_case 1
  cell $eq $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n70
  end
  attribute \full_case 1
  cell $eq $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n75
  end
  attribute \full_case 1
  cell $pmux $c17
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A \a
    connect \B { $n47 $n46 $n45 $n44 $n51 $n50 $n49 $n48 }
    connect \S { $n75 $n70 }
    connect \Y \t
  end
  attribute \full_case 1
  cell $eq $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n76
  end
  attribute \full_case 1
  cell $eq $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'00
    connect \Y $n81
  end
  attribute \full_case 1
  cell $pmux $c20
    parameter \WIDTH 4
    parameter \S_WIDTH 2
    connect \A { $n65 $n64 $n63 $n62 }
    connect \B { $n47 $n46 $n45 $n44 $n61 $n60 $n59 $n58 }
    connect \S { $n81 $n76 }
    connect \Y \y
  end
  attribute \full_case 1
  cell $mux $c21
    parameter \WIDTH 4
    connect \A \a
    connect \B { $n55 $n54 $n53 $n52 }
    connect \S \u
    connect \Y { $n85 $n84 $n83 $n82 }
  end
  attribute \full_case 1
  cell $eq $c22
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \s
    connect \B 2'01
    connect \Y $n86
  end
  attribute \full_case 1
  cell $mux $c24
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B { $n85 $n84 $n83 $n82 }
    connect \S $n86
    connect \Y { $n51 $n50 $n49 $n48 }
  end
end
