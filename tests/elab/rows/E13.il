# generated: elab-check --write
attribute \top 1
module \e13_replicate
  wire input 1 \a
  wire input 2 \b
  wire width 4 output 3 \y1
  wire width 4 output 4 \y2
  wire width 6 output 5 \y3
  wire width 2 output 6 \y4
  connect \y1 { \a \a \a \a }
  connect \y2 { \a \b \a \b }
  connect \y3 { \a \b \b \a \b \b }
  connect \y4 { \a \a }
end
