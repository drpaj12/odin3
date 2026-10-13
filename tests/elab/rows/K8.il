# generated: elab-check --write
attribute \top 1
module \k8_automatic
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 32 output 3 \y
  wire width 4 output 4 \q
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
  process $proc$K8_automatic_recursive.v:14$7
    assign { $n51 $n50 $n49 $n48 } \a
    assign { $n55 $n54 $n53 $n52 } \a
    assign { $n47 $n46 $n45 $n44 } \a
    sync posedge \clk
      update \q { $n47 $n46 $n45 $n44 }
  end
  connect \y 32'00000000000000000000000000011000
end
