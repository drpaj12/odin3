# generated: elab-check --write
attribute \top_module 1
attribute \version 32'00000000000000000000000000000001
attribute \top 1
module \l11_attributes
  attribute \clock 1
  wire input 1 \clk
  wire input 2 \a
  wire input 3 \b
  wire output 4 \y
  wire output 5 \z
  wire output 6 \q
  wire output 7 \r
  attribute \keep 1
  wire \w
  attribute \keep 1
  attribute \init "x"
  attribute \dont_touch 32'00000000000000000000000000000001
  wire \t
  wire $n10
  wire $n11
  wire $n12
  attribute \inst_attr 1
  cell \l11_leaf \u
    connect \d \a
    connect \e $n10
  end
  attribute \op_attr 1
  cell $and $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \w
  end
  attribute \unary_attr 1
  cell $not $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \w
    connect \Y \y
  end
  attribute \cond_attr 1
  cell $mux $c11
    parameter \WIDTH 1
    connect \A \w
    connect \B \b
    connect \S \a
    connect \Y \z
  end
  attribute \proc 1
  process $proc$L11_attributes.v:9$14
    assign $n12 \r
    assign $n11 \w
    switch \a
      case 1'0
        assign $n12 1'0
      case
        assign $n12 1'1
    end
    sync posedge \clk
      update \q $n11
      update \r $n12
  end
end
module \l11_leaf
  wire input 1 \d
  wire output 2 \e
  connect \e \d
end
