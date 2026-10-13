# generated: elab-check --write
attribute \top 1
module \k1_func_nonansi
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  wire $n19
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
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  cell $add $c4
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { $n38 $n37 $n36 $n35 $n34 }
  end
  cell $mux $c6
    parameter \WIDTH 4
    connect \A { $n37 $n36 $n35 $n34 }
    connect \B 4'1111
    connect \S $n38
    connect \Y { $n43 $n42 $n41 $n40 }
  end
  process $proc$K1_function_nonansi.v:12$7
    assign { $n20 $n19 $n18 $n17 } \a
    assign { $n24 $n23 $n22 $n21 } \b
    assign { $n29 $n28 $n27 $n26 $n25 } { $n38 $n37 $n36 $n35 $n34 }
    assign { $n33 $n32 $n31 $n30 } { $n43 $n42 $n41 $n40 }
    sync always
      update { $n16 $n15 $n14 $n13 } { $n33 $n32 $n31 $n30 }
  end
  connect \y { $n16 $n15 $n14 $n13 }
end
