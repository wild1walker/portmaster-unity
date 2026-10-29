using UnityEngine;

// The donor's game: a tiny game of pong (no assets: drawn with Unity's immediate-mode GUI). Up and
// down (or a touch) move the left paddle; the right one follows the ball. Its APK carries Unity's
// Android player, which ports of games made with the same Unity version run on (build.sh).
public class Pong : MonoBehaviour
{
    const float W = 640f, H = 480f, PW = 12f, PH = 80f, BS = 12f, MARGIN = 20f, SPEED = 300f;
    float left = H / 2f, right = H / 2f;
    Vector2 ball, vel;
    int scoreLeft, scoreRight;

    void Start()
    {
        Camera cam = Camera.main;
        if (cam != null) { cam.clearFlags = CameraClearFlags.SolidColor; cam.backgroundColor = Color.black; }
        Serve(1);
    }

    void Serve(int dir)
    {
        ball = new Vector2(W / 2f, H / 2f);
        vel = new Vector2(240f * dir, Random.Range(-160f, 160f));
    }

    void Update()
    {
        float dt = Time.deltaTime;
        float target = left - Input.GetAxisRaw("Vertical") * SPEED * dt;     // (y grows downwards here)
        if (Input.touchCount > 0)
            target = Mathf.MoveTowards(left, (1f - Input.GetTouch(0).position.y / Screen.height) * H, SPEED * dt);
        left = Mathf.Clamp(target, PH / 2f, H - PH / 2f);
        right = Mathf.Clamp(Mathf.MoveTowards(right, ball.y, 200f * dt), PH / 2f, H - PH / 2f);

        ball += vel * dt;
        if (ball.y < BS / 2f || ball.y > H - BS / 2f)
        {
            vel.y = -vel.y;
            ball.y = Mathf.Clamp(ball.y, BS / 2f, H - BS / 2f);
        }
        if (vel.x < 0f && ball.x - BS / 2f < MARGIN + PW && Mathf.Abs(ball.y - left) < PH / 2f)
            vel.x = -vel.x * 1.05f;
        if (vel.x > 0f && ball.x + BS / 2f > W - MARGIN - PW && Mathf.Abs(ball.y - right) < PH / 2f)
            vel.x = -vel.x * 1.05f;
        if (ball.x < 0f) { scoreRight++; Serve(1); }
        else if (ball.x > W) { scoreLeft++; Serve(-1); }
    }

    void OnGUI()
    {
        Box(MARGIN, left - PH / 2f, PW, PH);
        Box(W - MARGIN - PW, right - PH / 2f, PW, PH);
        Box(ball.x - BS / 2f, ball.y - BS / 2f, BS, BS);
        GUI.Label(new Rect(Screen.width / 2f - 40f, 10f, 80f, 30f), scoreLeft + " : " + scoreRight);
    }

    void Box(float x, float y, float w, float h)
    {
        float sx = Screen.width / W, sy = Screen.height / H;
        GUI.DrawTexture(new Rect(x * sx, y * sy, w * sx, h * sy), Texture2D.whiteTexture);
    }
}
